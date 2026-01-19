#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <mm/heap.h>

#define HEAP_START          0xFFFF800000000000ULL
#define HEAP_INITIAL_PAGES  64
#define HEAP_MAX_PAGES      8192
#define ALIGNMENT           8

typedef struct heap_block {
    size_t size;
    bool free;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *heap_head = NULL;
static uint8_t *heap_end = NULL;
static bool heap_ready = false;

static bool expand_heap(size_t pages) {
    if ((heap_end + pages * PAGE_SIZE) >
        ((uint8_t*)HEAP_START + HEAP_MAX_PAGES * PAGE_SIZE))
        return false;

    if (vmm_map_pages(
            kernel_page_map,
            heap_end,
            pages,
            PTE_PRESENT | PTE_WRITABLE
        ) != 0)
        return false;

    heap_end += pages * PAGE_SIZE;
    return true;
}

static void init_heap() {
    heap_head = (heap_block_t*)HEAP_START;
    heap_end = (uint8_t*)HEAP_START;

    if (!expand_heap(HEAP_INITIAL_PAGES))
        return;

    heap_head->size = HEAP_INITIAL_PAGES * PAGE_SIZE - sizeof(heap_block_t);
    heap_head->free = true;
    heap_head->next = NULL;

    heap_ready = true;
}

void *heap_allocate(size_t size) {
    if (!heap_ready)
        init_heap();

    size = ALIGN_UP(size, ALIGNMENT);

    heap_block_t *current = heap_head;

    while (current) {
        if (current->free && current->size >= size) {
            if (current->size >= size + sizeof(heap_block_t) + ALIGNMENT) {
                heap_block_t *new_block =
                    (heap_block_t*)((uint8_t*)(current + 1) + size);

                new_block->size =
                    current->size - size - sizeof(heap_block_t);
                new_block->free = true;
                new_block->next = current->next;

                current->size = size;
                current->next = new_block;
            }

            current->free = false;
            return (void*)(current + 1);
        }
        current = current->next;
    }

    size_t needed_pages =
        (size + sizeof(heap_block_t) + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!expand_heap(needed_pages))
        return NULL;

    heap_block_t *block =
        (heap_block_t*)(heap_end - needed_pages * PAGE_SIZE);

    block->size = needed_pages * PAGE_SIZE - sizeof(heap_block_t);
    block->free = false;
    block->next = NULL;

    current = heap_head;
    while (current->next)
        current = current->next;
    current->next = block;

    return (void*)(block + 1);
}

void heap_free(void *ptr) {
    if (!ptr)
        return;

    heap_block_t *block = ((heap_block_t*)ptr) - 1;
    if (block->free)
        return;

    block->free = true;

    if (block->next && block->next->free) {
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
    }

    heap_block_t *current = heap_head;
    while (current && current->next && current->next != block)
        current = current->next;

    if (current && current->free) {
        current->size += sizeof(heap_block_t) + block->size;
        current->next = block->next;
    }
}
