#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <vendor/limine_bootloader/limine.h>

#define PAGE_SIZE       4096

extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_hhdm_request hhdm_request;

typedef uint64_t paddr_t;

typedef struct BitmapEntry {
    struct BitmapEntry* next_entry;
    size_t available_pages;
    size_t capacity;
    size_t search_cursor;
    uint64_t bitmap[];
} bitmap_entry_t;

static inline void* paddr_to_vaddr(paddr_t paddr){
    return (void*)(paddr + hhdm_request.response->offset);
}

static inline paddr_t vaddr_to_paddr(void* paddr){
    return (paddr_t)((char*)paddr - hhdm_request.response->offset);
}

/// @brief allocate pages PMM
/// @param page_count page count (bytes / 4096) will allocate to the nearist 4096 bytes tho
/// @return page base ptr
paddr_t pmm_alloc_pages(size_t page_count);

/// @brief frees the page(s)
/// @param paddr base of the start of the free.
/// @param page_count ammount of pages to free
void pmm_free_pages(paddr_t paddr, size_t page_count);

/// @brief gets the size of physical memory available
/// @return unsigned 64 memory size in bytes
size_t paging_get_usable_mem_size();

/// @brief gets the highest physical address
/// @return highest paddr
uintptr_t get_max_paddr();

/// @brief Physical Memory Management
/// @return success
uint8_t pmm_init();