#include <vendor/limine_bootloader/limine.h>
#include <drivers/serial/serial.h>
#include <mm/pmm.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ansii.h>
#include <stddef.h>
#include <string.h>
#include <status.h>

#define FRAME_USED      1
#define FRAME_FREE      0

#define PAGE_ALIGN_DOWN(x)  (((x) / PAGE_SIZE) * PAGE_SIZE)
#define PAGE_ALIGN_UP(x)    ((((x) + (PAGE_SIZE-1)) / PAGE_SIZE) * PAGE_SIZE)

static bitmap_entry_t* bitmap_head = NULL;

uintptr_t get_max_paddr() {
    struct limine_memmap_response* mmap = memmap_request.response;
    uint64_t max = 0;

    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* entry = mmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t end = entry->base + entry->length;
        if (end > max) max = end;
    }

    return (uintptr_t)max;
}

size_t paging_get_usable_mem_size() {
    struct limine_memmap_response* mmap = memmap_request.response;
    size_t bytes = 0;

    for (size_t i = 0; i < mmap->entry_count; i++) {
        if (mmap->entries[i]->type == LIMINE_MEMMAP_USABLE)
            bytes += mmap->entries[i]->length;
    }
    return bytes;
}

void paging_mark_entry_unavailable(bitmap_entry_t* entry, size_t start, size_t page_count) {
    for (size_t i = 0; i < page_count; i++) {
        size_t index = start + i;
        entry->bitmap[index / 8] |= (FRAME_USED << (index % 8));
    }
    entry->available_pages -= page_count;
}

static void paging_mark_entry_available(bitmap_entry_t* entry, size_t start, size_t page_count) {
    for (size_t i = 0; i < page_count; i++) {
        size_t index = start + i;
        entry->bitmap[index / 8] &= ~(FRAME_USED << (index % 8));
    }
    entry->available_pages += page_count;
}

uint8_t pmm_init() {
    struct limine_memmap_response* mmap = memmap_request.response;
    struct limine_hhdm_response* hhdm = hhdm_request.response;

    bitmap_entry_t** prev_tail = &bitmap_head;

    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        if (mmap->entries[i]->type != LIMINE_MEMMAP_USABLE)
            continue;

        // store the bitmap safely 
        uintptr_t bitmap_phys = PAGE_ALIGN_UP(mmap->entries[i]->base + PAGE_SIZE); 
        bitmap_entry_t* bmentry = (bitmap_entry_t*)(bitmap_phys + hhdm->offset);

        *prev_tail = bmentry;
        bmentry->next_entry = NULL;
        prev_tail = &bmentry->next_entry;

        size_t total_pages = mmap->entries[i]->length / PAGE_SIZE;

        // Reserve space for bitmap itself
        size_t bitmap_bytes = sizeof(bitmap_entry_t) + ((total_pages + 7) / 8);
        size_t bitmap_pages = PAGE_ALIGN_UP(bitmap_bytes) / PAGE_SIZE;

        bmentry->capacity = total_pages - bitmap_pages;
        bmentry->available_pages = bmentry->capacity;
        bmentry->search_cursor = 0;

        memset(bmentry->bitmap, FRAME_FREE, (bmentry->capacity + 7) / 8);

        // Mark bitmap pages themselves
        paging_mark_entry_unavailable(bmentry, 0, bitmap_pages);

        serial_printf(LOG_INFO "Bitmap created at %p, %d usable pages\n", (void*)bmentry, bmentry->available_pages);
    }

    serial_printf(LOG_OK "PMM Initialized\n");
    return 0;
}

static int64_t paging_bitmap_find_free_in_range(bitmap_entry_t* entry, size_t count, size_t begin, size_t end) {
    size_t free_ext = 0;
    size_t start = 0;

    for (size_t i = begin; i < end; i++) {
        size_t byte = entry->bitmap[i / 8];
        if (!(byte & (1 << (i % 8)))) {
            if (free_ext == 0) start = i;
            free_ext++;
            if (free_ext == count) return start;
        } else {
            free_ext = 0;
        }
    }
    return -1;
}

static int64_t paging_bitmap_find_free(bitmap_entry_t* entry, size_t count) {
    if (!entry || !count || count > entry->capacity)
        return -1;

    size_t cursor = entry->search_cursor;
    if (cursor >= entry->capacity)
        cursor = 0;

    int64_t start = paging_bitmap_find_free_in_range(entry, count, cursor, entry->capacity);
    if (start < 0 && cursor != 0) {
        start = paging_bitmap_find_free_in_range(entry, count, 0, cursor);
    }

    if (start >= 0) {
        entry->search_cursor = (size_t)start + count;
        if (entry->search_cursor >= entry->capacity)
            entry->search_cursor = 0;
    }

    return start;
}

paddr_t pmm_alloc_pages(size_t page_count) {
    if (!page_count) return 0;
    struct limine_hhdm_response* hhdm = hhdm_request.response;

    for (bitmap_entry_t* head = bitmap_head; head != NULL; head = head->next_entry) {
        if (head->available_pages >= page_count) {
            int64_t start = paging_bitmap_find_free(head, page_count);
            if (start < 0) continue;

            paging_mark_entry_unavailable(head, start, page_count);
            uintptr_t paddr = ((uintptr_t)head - hhdm->offset) + (start * PAGE_SIZE);

            return paddr;
        }
    }

    return status_print_error(OUT_OF_MEMORY);
}

void pmm_free_pages(paddr_t paddr, size_t page_count) {
    struct limine_hhdm_response* hhdm = hhdm_request.response;

    for (bitmap_entry_t* entry = bitmap_head; entry != NULL; entry = entry->next_entry) {
        uintptr_t entry_start = (uintptr_t)entry - hhdm->offset;
        uintptr_t entry_end = entry_start + (entry->capacity * PAGE_SIZE);

        if (paddr >= entry_start && paddr < entry_end) {
            size_t start = (paddr - entry_start) / PAGE_SIZE;
            paging_mark_entry_available(entry, start, page_count);
            if (start < entry->search_cursor)
                entry->search_cursor = start;
            return;
        }
    }
}
