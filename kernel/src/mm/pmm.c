#include <boot/bootloader_interface/generic_bootloader.h>
#include <drivers/serial/serial.h>
#include <mm/pmm.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ansii.h>
#include <stddef.h>
#include <string.h>
#include <status.h>
#include <mm/paging.h>

#define FRAME_USED  1
#define FRAME_FREE  0

#define BIT_SET(bm, i)   ((bm)[(i) >> 3] |=  (uint8_t)(1u << ((i) & 7)))
#define BIT_CLR(bm, i)   ((bm)[(i) >> 3] &= ~(uint8_t)(1u << ((i) & 7)))
#define BIT_TST(bm, i)   ((bm)[(i) >> 3] &   (uint8_t)(1u << ((i) & 7)))

#define PMM_MIN_REGION_PAGES  4

static bitmap_entry_t *bitmap_head = NULL;

uintptr_t get_max_paddr(void) {
    uint64_t max = 0;
    for (size_t i = 0; i < g_gbi.memmap_entry_count; i++) {
        gbi_memmap_entry_t *e = &g_gbi.memmap[i];
        if (e->type != GBI_MEMMAP_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > max) max = end;
    }
    return (uintptr_t)max;
}

size_t paging_get_usable_mem_size(void) {
    size_t bytes = 0;
    for (size_t i = 0; i < g_gbi.memmap_entry_count; i++) {
        if (g_gbi.memmap[i].type == GBI_MEMMAP_USABLE)
            bytes += g_gbi.memmap[i].length;
    }
    return bytes;
}

void paging_mark_entry_unavailable(bitmap_entry_t *entry, size_t start, size_t page_count) {
    for (size_t i = 0; i < page_count; i++)
        BIT_SET(entry->bitmap, start + i);
    entry->available_pages -= page_count;
}

static void paging_mark_entry_available(bitmap_entry_t *entry, size_t start, size_t page_count) {
    for (size_t i = 0; i < page_count; i++)
        BIT_CLR(entry->bitmap, start + i);
    entry->available_pages += page_count;
}

uint8_t pmm_init(void) {
    bitmap_entry_t **prev_tail = &bitmap_head;

    for (size_t i = 0; i < g_gbi.memmap_entry_count; i++) {
        gbi_memmap_entry_t *region = &g_gbi.memmap[i];
        if (region->type != GBI_MEMMAP_USABLE)
            continue;

        uintptr_t region_base = PAGE_ALIGN_UP(region->base);
        uintptr_t region_end  = PAGE_ALIGN_DOWN(region->base + region->length);
        if (region_end <= region_base)
            continue;

        size_t total_pages = (region_end - region_base) / PAGE_SIZE;

        size_t header_bytes = sizeof(bitmap_entry_t) + ((total_pages + 7) / 8);
        size_t header_pages = (header_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        // waste more regions, we love this
        if (total_pages < header_pages + PMM_MIN_REGION_PAGES) {
            serial_printf(LOG_WARN "PMM: skipping tiny region 0x%lx (%zu pages)\n",
                          (unsigned long)region_base, total_pages);
            continue;
        }

        bitmap_entry_t *bm = (bitmap_entry_t *)(region_base + g_gbi.hhdm_offset);

        *prev_tail       = bm;
        bm->next_entry   = NULL;
        prev_tail        = &bm->next_entry;

        bm->region_base    = region_base;
        bm->header_pages   = header_pages;
        bm->capacity       = total_pages - header_pages;
        bm->available_pages = bm->capacity;
        bm->search_cursor  = 0;

        memset(bm->bitmap, 0, (total_pages + 7) / 8);

        paging_mark_entry_unavailable(bm, 0, header_pages);

        serial_printf(LOG_INFO "PMM: region 0x%lx-0x%lx  header=%zu  usable=%zu pages\n",
                      (unsigned long)region_base,
                      (unsigned long)region_end,
                      header_pages,
                      bm->available_pages);
    }

    serial_printf(LOG_OK "PMM: initialized, %zu MB free\n",
                  pmm_available_pages() * PAGE_SIZE / (1024 * 1024));
    return 0;
}

static int64_t paging_bitmap_find_free_in_range(bitmap_entry_t *entry,
                                                 size_t count,
                                                 size_t begin,
                                                 size_t end) {
    if (count == 0 || begin >= end)
        return -1;

    size_t free_run = 0;
    size_t run_start = 0;

    size_t i = begin;

    while (i < end) {
        uint8_t byte = entry->bitmap[i >> 3];
        if (byte == 0xFF) {
            free_run = 0;
            i = (i & ~(size_t)7) + 8;
            continue;
        }

        size_t bit_start = i & 7;
        size_t bit_end   = 8;
        if ((i & ~(size_t)7) + 8 > end)
            bit_end = end & 7 ? end & 7 : 8;

        for (size_t b = bit_start; b < bit_end; b++, i++) {
            if (!(byte & (uint8_t)(1u << b))) {
                if (free_run == 0) run_start = i;
                free_run++;
                if (free_run == count) return (int64_t)run_start;
            } else {
                free_run = 0;
            }
        }

        if (!(i & 7)) continue;
        i = (i & ~(size_t)7) + 8;
    }

    return -1;
}

static int64_t paging_bitmap_find_free(bitmap_entry_t *entry, size_t count) {
    if (!entry || !count || count > entry->capacity)
        return -1;

    size_t cursor = entry->search_cursor;
    if (cursor >= entry->capacity)
        cursor = 0;

    int64_t start = paging_bitmap_find_free_in_range(entry, count, cursor, entry->capacity);
    if (start < 0 && cursor != 0)
        start = paging_bitmap_find_free_in_range(entry, count, 0, cursor);

    if (start >= 0) {
        entry->search_cursor = (size_t)start + count;
        if (entry->search_cursor >= entry->capacity)
            entry->search_cursor = 0;
    }

    return start;
}

paddr_t pmm_alloc_pages(size_t page_count) {
    if (!page_count) return 0;

    for (bitmap_entry_t *bm = bitmap_head; bm != NULL; bm = bm->next_entry) {
        if (bm->available_pages < page_count)
            continue;

        int64_t start = paging_bitmap_find_free(bm, page_count);
        if (start < 0)
            continue;

        paging_mark_entry_unavailable(bm, (size_t)start, page_count);
        uintptr_t paddr = bm->region_base
                        + (bm->header_pages + (size_t)start) * PAGE_SIZE;
        return (paddr_t)paddr;
    }

    return (paddr_t)status_print_error(OUT_OF_MEMORY);
}

size_t pmm_available_pages(void) {
    size_t total = 0;
    for (bitmap_entry_t *e = bitmap_head; e != NULL; e = e->next_entry)
        total += e->available_pages;
    return total;
}

void pmm_free_pages(paddr_t paddr, size_t page_count) {
    for (bitmap_entry_t *bm = bitmap_head; bm != NULL; bm = bm->next_entry) {
        uintptr_t usable_start = bm->region_base + bm->header_pages * PAGE_SIZE;
        uintptr_t usable_end   = usable_start + bm->capacity * PAGE_SIZE;

        if ((uintptr_t)paddr < usable_start || (uintptr_t)paddr >= usable_end)
            continue;

        size_t idx = ((uintptr_t)paddr - usable_start) / PAGE_SIZE;

        if (idx + page_count > bm->capacity)
            page_count = bm->capacity - idx;

        paging_mark_entry_available(bm, idx, page_count);

        if (idx < bm->search_cursor)
            bm->search_cursor = idx;
        return;
    }

    serial_printf(LOG_WARN "PMM: pmm_free_pages called with unknown paddr 0x%lx\n",
                  (unsigned long)paddr);
}