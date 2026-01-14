#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint64_t vmm_cr3_t;

vmm_cr3_t vmm_create_address_space(void);

void vmm_destroy_address_space(vmm_cr3_t cr3);
void vmm_switch_address_space(vmm_cr3_t cr3);

int vmm_map_pages(vmm_cr3_t cr3, void* virt, size_t page_count, uint64_t flags);
void vmm_unmap_pages(vmm_cr3_t cr3, void* virt, size_t page_count);
void* vmm_alloc_pages(size_t pages);
void vmm_free_pages(void* addr, size_t pages);
