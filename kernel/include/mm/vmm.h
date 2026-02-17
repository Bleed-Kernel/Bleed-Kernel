#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mm/pmm.h>

typedef uint64_t vmm_cr3_t;

typedef struct vmm_region {
    uintptr_t base;
    size_t pages;
    struct vmm_region *next;
} vmm_region_t;

int vmm_map_pages(vmm_cr3_t cr3, void* virt, size_t page_count, uint64_t flags);
int vmm_map_phys_pages(vmm_cr3_t cr3, void *virt, paddr_t phys_start, size_t page_count, uint64_t flags);
void vmm_unmap_pages(vmm_cr3_t cr3, void* virt, size_t page_count);
int vmm_unmap_free_pages(vmm_cr3_t cr3, void *virt, size_t page_count);
int vmm_protect_pages(vmm_cr3_t cr3, void *virt, size_t page_count, uint64_t flags);
int vmm_translate(vmm_cr3_t cr3, void *virt, paddr_t *out_paddr);

void* vmm_alloc_pages(size_t pages);
void vmm_free_pages(void* addr, size_t pages);
