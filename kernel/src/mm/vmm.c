#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <mm/spinlock.h>
#include <mm/cow.h>

#define VMM_BASE_FLAGS (PTE_WRITABLE | PTE_NX)
#define VMM_NODES_PER_SLAB  (PAGE_SIZE / sizeof(vmm_region_t))

// dont get confused this is in the linker file
extern char __kernel_end;
extern paddr_t kernel_page_map;

static vmm_region_t *vmm_node_freelist = NULL;

static int vmm_slab_refill(void) {
    paddr_t slab_phys = pmm_alloc_pages(1);
    if (!slab_phys)
        return -1;

    vmm_region_t *slab = (vmm_region_t *)paddr_to_vaddr(slab_phys);
    memset(slab, 0, PAGE_SIZE);

    for (size_t i = 0; i < VMM_NODES_PER_SLAB; i++) {
        slab[i].next = vmm_node_freelist;
        vmm_node_freelist = &slab[i];
    }
    return 0;
}

static vmm_region_t *vmm_node_alloc(void) {
    if (!vmm_node_freelist) {
        if (vmm_slab_refill() != 0)
            return NULL;
    }
    vmm_region_t *n = vmm_node_freelist;
    vmm_node_freelist = n->next;
    memset(n, 0, sizeof(*n));
    return n;
}

static void vmm_node_free(vmm_region_t *n) {
    n->next = vmm_node_freelist;
    vmm_node_freelist = n;
}

static spinlock_t   vmm_lock;
static int          vmm_lock_init_done = 0;
static vmm_region_t *vmm_regions = NULL;
static uintptr_t    vmm_base_hint = 0;

static inline void vmm_init(void) {
    if (vmm_lock_init_done)
        return;
    spinlock_init(&vmm_lock);

    vmm_base_hint = PAGE_ALIGN_UP((uintptr_t)&__kernel_end + 64 * PAGE_SIZE);
    vmm_lock_init_done = 1;
}

static inline int vmm_overlap(uintptr_t a, size_t apages,
                               uintptr_t b, size_t bpages) {
    uintptr_t a_end = a + apages * PAGE_SIZE;
    uintptr_t b_end = b + bpages * PAGE_SIZE;
    return (a < b_end) && (b < a_end);
}

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

    vmm_region_t *node = vmm_node_alloc();
    if (!node)
        return 0;

    node->base  = candidate;
    node->pages = pages;
    if (!vmm_regions || candidate < vmm_regions->base) {
        node->next  = vmm_regions;
        vmm_regions = node;
    } else {
        vmm_region_t *prev = vmm_regions;
        while (prev->next && prev->next->base < candidate)
            prev = prev->next;
        node->next  = prev->next;
        prev->next  = node;
    }

    uintptr_t next_hint = PAGE_ALIGN_UP(candidate + pages * PAGE_SIZE);
    if (next_hint > vmm_base_hint)
        vmm_base_hint = next_hint;

    return candidate;
}

static int vmm_release_range_locked(uintptr_t base, size_t pages) {
    vmm_region_t *prev = NULL;
    for (vmm_region_t *r = vmm_regions; r; r = r->next) {
        if (r->base == base && r->pages == pages) {
            if (prev) prev->next = r->next;
            else      vmm_regions = r->next;
            vmm_node_free(r);
            return 0;
        }
        prev = r;
    }
    return -1;
}

static int vmm_page_paddr(vmm_cr3_t cr3, uintptr_t vaddr, paddr_t *out) {
    uint64_t *pte = paging_get_page(cr3, vaddr, 0);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;
    if (out)
        *out = (*pte & PADDR_ENTRY_MASK) | (vaddr & (PAGE_SIZE - 1));
    return 0;
}

int vmm_map_phys_pages(vmm_cr3_t cr3, void *virt, paddr_t phys_start,
                       size_t page_count, uint64_t flags) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    paddr_t   pa = phys_start & PADDR_ENTRY_MASK;
    uint64_t  mf = flags & ~PTE_PRESENT;

    for (size_t i = 0; i < page_count; i++)
        paging_map_page_invl(cr3, pa + i * PAGE_SIZE,
                             va + i * PAGE_SIZE, mf, 0);
    return 0;
}

int vmm_map_pages(vmm_cr3_t cr3, void *virt, size_t page_count, uint64_t flags) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    uint64_t  mf = flags & ~PTE_PRESENT;

    for (size_t i = 0; i < page_count; i++) {
        paddr_t phys = pmm_alloc_pages(1);
        if (!phys) {
            vmm_unmap_free_pages(cr3, (void *)va, i);
            return -1;
        }
        paging_map_page_invl(cr3, phys, va + i * PAGE_SIZE, mf, 0);
    }
    return 0;
}

void vmm_unmap_pages(vmm_cr3_t cr3, void *virt, size_t page_count) {
    if (!virt || !page_count)
        return;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    for (size_t i = 0; i < page_count; i++)
        paging_unmap_page(cr3, va + i * PAGE_SIZE);
}

int vmm_unmap_free_pages(vmm_cr3_t cr3, void *virt, size_t page_count) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va = PAGE_ALIGN_DOWN((uintptr_t)virt);
    for (size_t i = 0; i < page_count; i++) {
        uintptr_t vpage = va + i * PAGE_SIZE;
        uint64_t *pte   = paging_get_page(cr3, vpage, 0);
        if (!pte || !(*pte & PTE_PRESENT))
            continue;

        paddr_t ppage = *pte & PADDR_ENTRY_MASK;
        if (*pte & PTE_NOFREE) {
            // mmio, never came from the pmm
        } else if (*pte & PTE_COW) {
            if (cow_unref_page(ppage) == 0)
                pmm_free_pages(ppage, 1);
        } else {
            pmm_free_pages(ppage, 1);
        }
        paging_unmap_page(cr3, vpage);
    }
    return 0;
}

int vmm_protect_pages(vmm_cr3_t cr3, void *virt, size_t page_count, uint64_t flags) {
    if (!virt || !page_count)
        return -1;

    uintptr_t va        = PAGE_ALIGN_DOWN((uintptr_t)virt);
    uint64_t  keep_mask = PADDR_ENTRY_MASK | PTE_PRESENT | PTE_ACCESSED
                        | PTE_DIRTY | PTE_GLOBAL;
    uint64_t  set_flags = flags & ~(PTE_PRESENT | PADDR_ENTRY_MASK);

    for (size_t i = 0; i < page_count; i++) {
        uint64_t *pte = paging_get_page(cr3, va + i * PAGE_SIZE, 0);
        if (!pte || !(*pte & PTE_PRESENT))
            return -1;
        *pte = (*pte & keep_mask) | set_flags;
        asm volatile("invlpg (%0)" :: "r"(va + i * PAGE_SIZE) : "memory");
    }
    return 0;
}

int vmm_translate(vmm_cr3_t cr3, void *virt, paddr_t *out_paddr) {
    if (!virt || !out_paddr)
        return -1;
    return vmm_page_paddr(cr3, (uintptr_t)virt, out_paddr);
}

void *vmm_alloc_pages(size_t pages) {
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

void vmm_free_pages(void *addr, size_t pages) {
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