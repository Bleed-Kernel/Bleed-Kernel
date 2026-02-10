#include <stdint.h>
#include <stddef.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

typedef paddr_t vmm_cr3_t;

int vmm_map_pages(vmm_cr3_t cr3, void* virt, size_t page_count, uint64_t flags) {
    uintptr_t va = (uintptr_t)virt;
    for (size_t i = 0; i < page_count; i++) {
        paddr_t phys = pmm_alloc_pages(1);
        if (!phys) return -1;

        paging_map_page(cr3, phys, va + i * PAGE_SIZE, flags | PTE_NX);
    }
    return 0;
}

void vmm_unmap_pages(vmm_cr3_t cr3, void* virt, size_t page_count) {
    uintptr_t va = (uintptr_t)virt;
    for (size_t i = 0; i < page_count; i++) {
        paging_map_page(cr3, 0, va + i * PAGE_SIZE, PTE_NX);
    }
}

vmm_cr3_t vmm_create_address_space() {
    return paging_create_address_space();
}

void vmm_destroy_address_space(vmm_cr3_t cr3) {
    paging_destroy_address_space(cr3);
}

void vmm_switch_address_space(vmm_cr3_t cr3) {
    paging_switch_address_space(cr3);
}

void* vmm_alloc_pages(size_t pages) {
    paddr_t phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;
    void* vaddr = paddr_to_vaddr(phys);
    for (size_t i = 0; i < pages * PAGE_SIZE; i++) ((uint8_t*)vaddr)[i] = 0;
    return vaddr;
}

void vmm_free_pages(void* addr, size_t pages) {
    pmm_free_pages(vaddr_to_paddr(addr), pages);
}
