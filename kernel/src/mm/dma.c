#include <mm/dma.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>

void *dma_alloc(size_t pages, paddr_t *phys, bool low_only) {
    paddr_t page = pmm_alloc_pages(pages);
    if (!page)
        return NULL;

    if (low_only && (((uint64_t)page + pages * PAGE_SIZE - 1) >> 32)) {
        serial_printf(LOG_ERROR "dma: memory landed above 4G (0x%llx) and the device cant reach it\n",
                      (uint64_t)page);
        pmm_free_pages(page, pages);
        return NULL;
    }

    void *virt = paddr_to_vaddr(page);
    memset(virt, 0, pages * PAGE_SIZE);
    *phys = page;
    return virt;
}

void dma_free(paddr_t phys, size_t pages) {
    if (phys && pages)
        pmm_free_pages(phys, pages);
}
