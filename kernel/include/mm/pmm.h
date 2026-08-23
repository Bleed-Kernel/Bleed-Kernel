#pragma once
#include <stdint.h>
#include <stddef.h>
#include <boot/bootloader_interface/generic_bootloader.h>

typedef uintptr_t paddr_t;

static inline void *paddr_to_vaddr(paddr_t p) {
    return (void *)(p + g_gbi.hhdm_offset);
}

static inline paddr_t vaddr_to_paddr(void *v) {
    return (paddr_t)((uintptr_t)v - g_gbi.hhdm_offset);
}

typedef struct bitmap_entry {
    struct bitmap_entry *next_entry;

    uintptr_t   region_base;
    size_t      header_pages;
    size_t      capacity;
    size_t      available_pages;
    size_t      search_cursor;

    uint8_t     bitmap[];
} bitmap_entry_t;

uint8_t   pmm_init(void);
paddr_t   pmm_alloc_pages(size_t page_count);
void      pmm_free_pages(paddr_t paddr, size_t page_count);
size_t    pmm_available_pages(void);

uintptr_t get_max_paddr(void);
size_t    paging_get_usable_mem_size(void);

void paging_mark_entry_unavailable(bitmap_entry_t *entry, size_t start, size_t page_count);