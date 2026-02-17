#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <mm/spinlock.h>

#define VMM_BASE_FLAGS (PTE_WRITABLE | PTE_NX)

extern char __kernel_end;

static spinlock_t   vmm_lock;
static int          vmm_lock_init_done = 0;
static vmm_region_t *vmm_regions = NULL;
static uintptr_t    vmm_base_hint = 0;

static inline void vmm_init(void) {
    if (vmm_lock_init_done)
        return;
 
    spinlock_init(&vmm_lock);
    vmm_base_hint = PAGE_ALIGN_UP((uintptr_t)&__kernel_end + (16 * PAGE_SIZE));
    vmm_lock_init_done = 1;
}

/// @brief are two pages overlapping?
/// @param a target
/// @param apages width
/// @param b target
/// @param bpages width
/// @return success
static inline int vmm_overlap(uintptr_t a, uintptr_t apages, uintptr_t b, uintptr_t bpages) {
    uintptr_t a_end = a + apages * PAGE_SIZE;
    uintptr_t b_end = b + bpages * PAGE_SIZE;
    return (a < b_end) && (b < a_end);
}

/// @brief reserve a range of contiguous memory, assumes the caller has the locks
/// @param pages size
/// @return pointer
static uintptr_t vmm_reserve_range_locked(size_t pages) {
    if (!pages)
        return 0;

    uintptr_t candidate = vmm_base_hint;
    if (candidate < PAGE_SIZE)
        candidate = PAGE_SIZE;

    for (;;) {
        int collided = 0;
        for (vmm_region_t *r = vmm_regions; r; r = r->next) {
            if (vmm_overlap(candidate, pages, r->base, r->pages)) {
                candidate = PAGE_ALIGN_UP(r->base + r->pages * PAGE_SIZE);
                collided = 1;
                break;
            }
        }
        if (!collided)
            break;
    }

    vmm_region_t *node = (vmm_region_t *)paddr_to_vaddr(pmm_alloc_pages(1)); // just 4KiB nodes
    if (!node)
        return 0;
    memset(node, 0, sizeof(*node));
    node->base = candidate;
    node->pages = pages;

    if (!vmm_regions || candidate < vmm_regions->base) {
        node->next = vmm_regions;
        vmm_regions = node;
    } else {
        vmm_region_t *prev = vmm_regions;
        while (prev->next && prev->next->base < candidate)
            prev = prev->next;
        node->next = prev->next;
        prev->next = node;
    }

    vmm_base_hint = PAGE_ALIGN_UP(candidate + pages * PAGE_SIZE);
    return candidate;
}

/// @brief frees vmm pages in the range assuming the caller has locks
/// @param base 
/// @param pages 
/// @return 
static int vmm_release_range_locked(uintptr_t base, size_t pages) {
    vmm_region_t *prev = NULL;
    for (vmm_region_t *r = vmm_regions; r; r = r->next) {
        if (r->base == base && r->pages == pages) {
            if (prev) prev->next = r->next;
            else vmm_regions = r->next;
            pmm_free_pages(vaddr_to_paddr(r), 1);
            return 0;
        }
        prev = r;
    }
    return -1;
}

/// @brief gets the physical address of a page
/// @param cr3 address space
/// @param vaddr page vaddr
/// @param out out buffer
/// @return success
static int vmm_page_paddr(vmm_cr3_t cr3, uintptr_t vaddr, paddr_t *out) {
    uint64_t *pte = paging_get_page(cr3, vaddr, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    if (out)
        *out = (*pte & PADDR_ENTRY_MASK) | (vaddr & (PAGE_SIZE - 1));
    return 0;
}

/// @brief maps phsyical pages of size page count at virt
/// @param cr3 address space
/// @param virt virtual address
/// @param phys_start phsyical address start
/// @param page_count contigious size
/// @param flags PTE flags
/// @return success
int vmm_map_phys_pages(vmm_cr3_t cr3, void *virt, paddr_t phys_start, size_t page_count, uint64_t flags) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    paddr_t pa = phys_start & PADDR_ENTRY_MASK;
    uint64_t map_flags = flags & ~PTE_PRESENT;

    for (size_t i = 0; i < page_count; i++) {
        paging_map_page_invl(cr3, pa + (i * PAGE_SIZE), va + (i * PAGE_SIZE), map_flags, 0);
    }
    return 0;
}

/// @brief maps page_count virtual pages
/// @param cr3 address space
/// @param virt virtual target
/// @param page_count size
/// @param flags PTE Flags
/// @return success
int vmm_map_pages(vmm_cr3_t cr3, void* virt, size_t page_count, uint64_t flags) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    uint64_t map_flags = flags & ~PTE_PRESENT;

    for (size_t i = 0; i < page_count; i++) {
        paddr_t phys = pmm_alloc_pages(1);
        if (!phys) {
            vmm_unmap_free_pages(cr3, (void *)va, i);
            return -1;
        }
        paging_map_page_invl(cr3, phys, va + i * PAGE_SIZE, map_flags, 0);
    }
    return 0;
}

/// @brief unmaps virtual pages
/// @param cr3 address space
/// @param virt target
/// @param page_count size
void vmm_unmap_pages(vmm_cr3_t cr3, void* virt, size_t page_count) {
    if (!virt || !page_count)
        return;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    for (size_t i = 0; i < page_count; i++) {
        paging_unmap_page(cr3, va + i * PAGE_SIZE);
    }
}

/// @brief unmaps virtual pages and frees them in the PMM
/// @param cr3 address space
/// @param virt target address
/// @param page_count size
/// @return success
int vmm_unmap_free_pages(vmm_cr3_t cr3, void *virt, size_t page_count) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    for (size_t i = 0; i < page_count; i++) {
        uintptr_t vpage = va + i * PAGE_SIZE;
        paddr_t ppage = 0;
        if (vmm_page_paddr(cr3, vpage, &ppage) == 0) {
            pmm_free_pages(ppage & PADDR_ENTRY_MASK, 1);
            paging_unmap_page(cr3, vpage);
        }
    }
    return 0;
}

/// @brief update protection flags for a given virtual address
/// @param cr3 address space
/// @param virt virtual address
/// @param page_count size
/// @param flags new set flags
/// @return success
int vmm_protect_pages(vmm_cr3_t cr3, void *virt, size_t page_count, uint64_t flags) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    uint64_t keep_mask = PADDR_ENTRY_MASK | PTE_PRESENT | PTE_ACCESSED | PTE_DIRTY | PTE_GLOBAL;
    uint64_t set_flags = flags & ~(PTE_PRESENT | PADDR_ENTRY_MASK);

    for (size_t i = 0; i < page_count; i++) {
        uint64_t *pte = paging_get_page(cr3, va + i * PAGE_SIZE, 0);
        if (!pte || !(*pte & PTE_PRESENT))
            return -1;
        uint64_t base = *pte & keep_mask;
        *pte = base | set_flags;
        asm volatile("invlpg (%0)" :: "r"(va + i * PAGE_SIZE) : "memory");
    }
    return 0;
}

/// @brief translates a virtual address to a physical address, thin wrapper for vmm_page_paddr
/// @param cr3 address space
/// @param virt target
/// @param out_paddr result pointer
/// @return success
int vmm_translate(vmm_cr3_t cr3, void *virt, paddr_t *out_paddr) {
    if (!virt || !out_paddr)
        return -1;
    return vmm_page_paddr(cr3, (uintptr_t)virt, out_paddr);
}

/// @brief allocates pages in the VMM
/// @param pages size
/// @return pointer to base of page allocation
void* vmm_alloc_pages(size_t pages) {
    if (!pages)
        return NULL;

    vmm_init();
    unsigned long irq = irq_push();
    spinlock_acquire(&vmm_lock);
        uintptr_t base = vmm_reserve_range_locked(pages);
    spinlock_release(&vmm_lock);
    irq_restore(irq);

    if (!base)
        return NULL;

    if (vmm_map_pages(kernel_page_map, (void *)base, pages, VMM_BASE_FLAGS) != 0) {
        irq = irq_push();
        spinlock_acquire(&vmm_lock);
            (void)vmm_release_range_locked(base, pages);
        spinlock_release(&vmm_lock);
        irq_restore(irq);
        return NULL;
    }

    memset((void *)base, 0, pages * PAGE_SIZE);
    return (void *)base;
}

/// @brief frees Virtual Pages
/// @param addr base address
/// @param pages size to free
void vmm_free_pages(void* addr, size_t pages) {
    if (!addr || !pages)
        return;

    uintptr_t base = PAGE_ALIGN_DOWN((uintptr_t)addr);

    vmm_init();
    unsigned long irq = irq_push();
    spinlock_acquire(&vmm_lock);
        int rel = vmm_release_range_locked(base, pages);
    spinlock_release(&vmm_lock);
    irq_restore(irq);

    if (rel != 0)
        return;

    (void)vmm_unmap_free_pages(kernel_page_map, (void *)base, pages);
}
