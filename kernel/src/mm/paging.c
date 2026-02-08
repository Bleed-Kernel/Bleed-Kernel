#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <drivers/framebuffer/framebuffer.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <ansii.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/serial/serial.h>
#include <panic.h>
#include <sched/scheduler.h>

paddr_t kernel_page_map = 0;

extern volatile struct limine_memmap_request memmap_request;

// replace this with a standard wrmsr and rmsrs later they got reverted
static inline void write_msr(uint32_t msr, uint64_t val) {
    asm volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void pat_enable_wc(void) {
    uint64_t pat = read_msr(0x277);

    pat &= ~(0xFFULL << 32);
    pat |=  (0x01ULL << 32);

    write_msr(0x277, pat);
}

/// @brief allocate an empty page frame and return the paddr
/// @param vaddr out virtual address
/// @return physical address
uint64_t paging_alloc_empty_frame(void **vaddr) {
    paddr_t paddr = pmm_alloc_pages(1);
    if (!paddr)
        kprintf(LOG_ERROR "Page Allocation Failed\n");

    void *v = paddr_to_vaddr(paddr);
    if (v) memset(v, 0, PAGE_SIZE_4K);
    if (vaddr) *vaddr = v;
    return paddr;
}
/// @brief write a page table at a given index, if it already exsists, we return its paddr
/// @param table Pointer to target table
/// @param index Index of entry to modify
/// @param flags PTE Flags
/// @return paddr table[index]
static uint64_t paging_write_table_entry(uint64_t* table, size_t index, uint64_t flags) {
    uint64_t entry = table[index];
    if (entry & PTE_PRESENT) return entry & PADDR_ENTRY_MASK;

    void *v = NULL;
    uint64_t p = paging_alloc_empty_frame(&v);
    if (!v) return 0;

    table[index] = (p & PADDR_ENTRY_MASK) | (flags & ~PADDR_ENTRY_MASK);
    return p & PADDR_ENTRY_MASK;
}

/// @brief walk page tables at PD level for a vaddr
/// @param vaddr vaddr to resolve
/// @param out_pd page directory pointer to resolved vaddr
/// @param out_pd_index index of the output page directory for vaddr
void paging_walk_page_tables(paddr_t cr3, uint64_t vaddr,
                             uint64_t **out_pd, size_t *out_pd_index,
                             uint64_t flags) {
    uint64_t *pml4 = paddr_to_vaddr(cr3 & PADDR_ENTRY_MASK);

    size_t pml4i = (vaddr >> 39) & 0x1FF;
    size_t pdpti = (vaddr >> 30) & 0x1FF;
    size_t pdi   = (vaddr >> 21) & 0x1FF;
    size_t pti   = (vaddr >> 12) & 0x1FF;

    uint64_t p_pdpt = paging_write_table_entry(
        pml4, pml4i, PTE_PRESENT | PTE_WRITABLE | flags);
    uint64_t *pdpt = paddr_to_vaddr(p_pdpt);

    uint64_t p_pd = paging_write_table_entry(
        pdpt, pdpti, PTE_PRESENT | PTE_WRITABLE | flags);
    uint64_t *pd = paddr_to_vaddr(p_pd);

    if (!(flags & PTE_PS)) {
        uint64_t p_pt = paging_write_table_entry(
            pd, pdi, PTE_PRESENT | PTE_WRITABLE | flags);
        uint64_t *pt = paddr_to_vaddr(p_pt);
        *out_pd = pt;
        *out_pd_index = pti;
    } else {
        *out_pd = pd;
        *out_pd_index = pdi;
    }
}

/// @brief map a physical page at a vaddr using a pd entry
/// @param paddr physical address to map the page frame at
/// @param vaddr virtual address to map the page at
/// @param flags PTE Flags
void paging_map_page(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags) {
    uint64_t *pd;
    size_t idx;

    paging_walk_page_tables(cr3, vaddr, &pd, &idx,
        flags & (PTE_USER | PTE_PS));

    if (!pd) return;

    pd[idx] = (paddr & PADDR_ENTRY_MASK)
            | (flags & ~PADDR_ENTRY_MASK)
            | PTE_PRESENT;

    asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/// @brief create the kernels page map and save it, key part of multitasking
void paging_init_kernel_map(void) {
    kernel_page_map = read_cr3() & PADDR_ENTRY_MASK;
    if (!paddr_to_vaddr(kernel_page_map))
        ke_panic("Kernel PML4 unmapped");
}

/// @brief reinitalise paging so we can access a full memory range, not just the
/// default from limine
void reinit_paging(void) {
    struct limine_memmap_response *mmap = memmap_request.response;

    uintptr_t fb_base = (uintptr_t)framebuffer_get_addr(0);
    size_t fb_size =
        framebuffer_get_height(0) * framebuffer_get_pitch(0);
    uintptr_t fb_end = fb_base + fb_size;

    for (size_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = e->base & ~(PAGE_SIZE_2M - 1);
        uint64_t end  = e->base + e->length;

        for (uint64_t p = base; p + PAGE_SIZE_2M <= end; p += PAGE_SIZE_2M) {
            if (p >= fb_base && p < fb_end)
                continue;

            void *hv = paddr_to_vaddr(p);
            if (!hv) continue;

            paging_map_page(
                read_cr3(), p, (uintptr_t)hv,
                PAGE_KERNEL_RW | PTE_PS
            );
        }
    }

    pat_enable_wc();

    uintptr_t fb_p = fb_base & ~(PAGE_SIZE_2M - 1);
    uintptr_t fb_p_end =
        (fb_end + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);

    for (uintptr_t p = fb_p; p < fb_p_end; p += PAGE_SIZE_2M) {
        paging_map_page(
            read_cr3(), p, (uintptr_t)paddr_to_vaddr(p),
            PAGE_KERNEL_RW | PTE_PS | PTE_PS_PAT
        );
    }

    paging_init_kernel_map();
}

/// @brief reinitalise paging so we can access a full memory range, not just the
/// default from limine
paddr_t paging_create_address_space(void){
    void* vaddr = NULL;
    paddr_t pml4_paddr = paging_alloc_empty_frame(&vaddr);

    if (!pml4_paddr) {
        serial_printf("Failed to allocate PML4\n"); 
        return 0;
    }
    
    uint64_t *kernel_pml4 = (uint64_t *)paddr_to_vaddr(kernel_page_map);
    uint64_t *new_pml4 = (uint64_t *)vaddr;

    memset(new_pml4, 0, PAGE_SIZE);

    for (size_t i = 256; i < 512; i++){
        new_pml4[i] = kernel_pml4[i];
    }
    
    return pml4_paddr;
}

void paging_unmap_page(paddr_t cr3, uint64_t vaddr) {
    uint64_t *pte = paging_get_page(cr3, vaddr, 0);
    if (!pte)
        return;

    if (!(*pte & PTE_PRESENT))
        return;

    *pte = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/// @brief switch the current CR3 address space context
/// @param cr3 cr3 paddr
void paging_switch_address_space(paddr_t cr3){
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/// @brief free address space CR3 provided
/// @param cr3 target
void paging_destroy_address_space(paddr_t cr3){
    if (!cr3) return;
    pmm_free_pages(cr3, 1);
}

uint64_t* paging_get_page(paddr_t cr3, uint64_t vaddr, int create) {
    uint64_t *pml4 = (uint64_t*)paddr_to_vaddr(cr3 & PADDR_ENTRY_MASK);
    if (!pml4) return NULL;

    size_t pml4_index = (vaddr >> 39) & 0x1FF;
    size_t pdpt_index = (vaddr >> 30) & 0x1FF;
    size_t pd_index   = (vaddr >> 21) & 0x1FF;
    size_t pt_index   = (vaddr >> 12) & 0x1FF;

    uint64_t *pdpt = (uint64_t*)paddr_to_vaddr(pml4[pml4_index] & PADDR_ENTRY_MASK);
    if (!pdpt) {
        if (!create) return NULL;
        paddr_t paddr = paging_alloc_empty_frame((void**)&pdpt);
        if (!pdpt) return NULL;
        pml4[pml4_index] = paddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    uint64_t *pd = (uint64_t*)paddr_to_vaddr(pdpt[pdpt_index] & PADDR_ENTRY_MASK);
    if (!pd) {
        if (!create) return NULL;
        paddr_t paddr = paging_alloc_empty_frame((void**)&pd);
        if (!pd) return NULL;
        pdpt[pdpt_index] = paddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    uint64_t *pt = (uint64_t*)paddr_to_vaddr(pd[pd_index] & PADDR_ENTRY_MASK);
    if (!pt) {
        if (!create) return NULL;
        paddr_t paddr = paging_alloc_empty_frame((void**)&pt);
        if (!pt) return NULL;
        pd[pd_index] = paddr | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    }

    return &pt[pt_index];
}