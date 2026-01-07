#include <stdint.h>
#include <mm/paging.h>
#include <stddef.h>
#include <string.h>
#include <status.h>

#define VM_USER_START 0x40000000ULL
#define VM_USER_END   0x8000000000ULL
#define VM_PAGE_COUNT ((VM_USER_END - VM_USER_START) / PAGE_SIZE_4K)

static uint8_t *vm_bitmap = NULL;


void init_vmm(void) {
    size_t bitmap_bytes = (VM_PAGE_COUNT + 7) / 8;
    vm_bitmap = (uint8_t*)pmm_alloc_pages((bitmap_bytes + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
    memset(vm_bitmap, 0, bitmap_bytes);
}

void* vm_alloc(size_t pages) {
    if (!pages || !vm_bitmap) return NULL;

    size_t free_run = 0;
    size_t start_index = 0;

    for (size_t i = 0; i < VM_PAGE_COUNT; i++) {
        size_t byte = vm_bitmap[i / 8];
        int bit = (byte >> (i % 8)) & 1;

        if (!bit) {
            if (free_run == 0) start_index = i;
            free_run++;
            if (free_run == pages) {
                for (size_t j = 0; j < pages; j++)
                    vm_bitmap[(start_index + j) / 8] |= (1 << ((start_index + j) % 8));

                return (void*)(VM_USER_START + start_index * PAGE_SIZE_4K);
            }
        } else {
            free_run = 0;
        }
    }

    return NULL;
}

void vm_free(void* vaddr, size_t pages) {
    if (!vaddr || !pages) return;
    uintptr_t addr = (uintptr_t)vaddr;

    if (addr < VM_USER_START || addr >= VM_USER_END) return;

    size_t start_index = (addr - VM_USER_START) / PAGE_SIZE_4K;

    for (size_t i = 0; i < pages; i++) {
        vm_bitmap[(start_index + i) / 8] &= ~(1 << ((start_index + i) % 8));
    }
}

