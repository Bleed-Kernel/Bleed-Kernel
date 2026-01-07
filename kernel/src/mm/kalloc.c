#include <mm/pmm.h>
#include <stddef.h>
#include <drivers/serial/serial.h>

/// @brief allocate BYTES (rounded up to the nearist 4kib)
/// @param bytes bytes (will be rounded to nearist 4kib)
/// @return allocated virtual address
void* kmalloc(size_t bytes){
    paddr_t p = pmm_alloc_pages((bytes + (PAGE_SIZE - 1)) / PAGE_SIZE);
    if (p == 0){
        return NULL;
    }

    return paddr_to_vaddr(p);
}

/// @brief free memory at address of size bytes (to the nearist upper 4kib)
/// @param addr address
/// @param bytes size to free
void kfree(void* addr, size_t bytes){
    pmm_free_pages(vaddr_to_paddr(addr), (bytes + (PAGE_SIZE - 1)) / PAGE_SIZE);
}