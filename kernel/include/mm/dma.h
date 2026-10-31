#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <mm/pmm.h>

// zeroed contiguous pages for a device to dma into, low_only keeps them under 4G
void *dma_alloc(size_t pages, paddr_t *phys, bool low_only);
void  dma_free(paddr_t phys, size_t pages);
