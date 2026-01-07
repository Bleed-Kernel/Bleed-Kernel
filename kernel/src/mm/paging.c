#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <ansii.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/serial/serial.h>
#include <panic.h>
#include <sched/scheduler.h>

paddr_t kernel_page_map = 0;

extern volatile struct limine_memmap_request memmap_request;

/// @brief allocate an empty page frame and return the paddr
/// @param vaddr out virtual address
/// @return physical address
uint64_t paging_alloc_empty_frame(void **vaddr) {
    paddr_t paddr = pmm_alloc_pages(1);
    if(!paddr)
        kprintf(LOG_ERROR "Page Allocation Failed, Out of Memory?\n");

    void* v = paddr_to_vaddr(paddr);
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

    void* new_entry_vaddr = NULL;
    uint64_t new_entry_paddr = paging_alloc_empty_frame(&new_entry_vaddr);
    if (!new_entry_vaddr) return 0;

    table[index] = (new_entry_paddr & PADDR_ENTRY_MASK) | (flags & ~PADDR_ENTRY_MASK);
    return new_entry_paddr & PADDR_ENTRY_MASK;
}

/// @brief walk page tables at PD level for a vaddr
/// @param vaddr vaddr to resolve
/// @param out_pd page directory pointer to resolved vaddr
/// @param out_pd_index index of the output page directory for vaddr
void paging_walk_page_tables(paddr_t cr3, uint64_t vaddr, uint64_t **out_pd, size_t *out_pd_index, uint64_t flags) {
    uint64_t ncr3 = cr3 & PADDR_ENTRY_MASK;
    uint64_t* pml4_vaddr = (uint64_t*)paddr_to_vaddr(ncr3);

    size_t pml4_index = (vaddr >> 39) & 0x1FF;
    size_t pdpt_index = (vaddr >> 30) & 0x1FF;
    size_t pd_index   = (vaddr >> 21) & 0x1FF;
    size_t pt_index   = (vaddr >> 12) & 0x1FF;

    uint64_t p_pdpt = paging_write_table_entry(pml4_vaddr, pml4_index, PTE_PRESENT | PTE_WRITABLE | flags);
    uint64_t* pdpt = (uint64_t*)paddr_to_vaddr(p_pdpt);

    uint64_t p_pd = paging_write_table_entry(pdpt, pdpt_index, PTE_PRESENT | PTE_WRITABLE | flags);
    uint64_t* pd = (uint64_t*)paddr_to_vaddr(p_pd);

    if (!(flags & PTE_PS)) {
        uint64_t p_pt = paging_write_table_entry(pd, pd_index, PTE_PRESENT | PTE_WRITABLE | flags);
        uint64_t* pt = (uint64_t*)paddr_to_vaddr(p_pt);

        *out_pd = pt;
        *out_pd_index = pt_index;
    }else{
        *out_pd = pd;
        *out_pd_index = pd_index;
    }
}

/// @brief map a physical page at a vaddr using a pd entry
/// @param paddr physical address to map the page frame at
/// @param vaddr virtual address to map the page at
/// @param flags PTE Flags
void paging_map_page(paddr_t cr3, uint64_t paddr, uint64_t vaddr, uint64_t flags) {
    uint64_t *pd;
    size_t pd_index;

    if (flags & PTE_PS){
        paging_walk_page_tables(cr3, vaddr, &pd, &pd_index, flags & (PTE_USER | PTE_PS));
    }else{
        paging_walk_page_tables(cr3, vaddr, &pd, &pd_index, flags & PTE_USER);
    }
    if (!pd) return;

    pd[pd_index] = (paddr & PADDR_ENTRY_MASK) | (flags & ~PADDR_ENTRY_MASK) | PTE_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/// @brief create the kernels page map and save it, key part of multitasking
void paging_init_kernel_map(void) {
    kernel_page_map = read_cr3() & PADDR_ENTRY_MASK;

    void *pml4_vaddr = paddr_to_vaddr(kernel_page_map);
    if (!pml4_vaddr) 
        ke_panic("Kernel PML4 not mapped");
    serial_printf("Kernel Page Map = %p\n", (void*)kernel_page_map);
}

/// @brief reinitalise paging so we can access a full memory range, not just the
/// default from limine
void reinit_paging() {
    struct limine_memmap_response* mmap = memmap_request.response;

    for (size_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* entry = mmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t mapped = 0;
        uint64_t base = entry->base & ~(PAGE_SIZE_2M - 1);
        uint64_t end = entry->base + entry->length;

        for (uint64_t paddr = base; paddr + PAGE_SIZE_2M <= end; paddr += PAGE_SIZE_2M) {
            void* hv = paddr_to_vaddr(paddr);
            if (!hv) continue;

            paging_map_page(read_cr3(), paddr, (paddr_t)hv, PAGE_KERNEL_RW | PTE_PS);
            mapped++;
        }
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