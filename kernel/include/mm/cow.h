#pragma once

#include <stdint.h>
#include <mm/pmm.h>

void cow_init(void);

void cow_ref_page(paddr_t phys);
uint32_t cow_unref_page(paddr_t phys);
uint32_t cow_get_refcount(paddr_t phys);
