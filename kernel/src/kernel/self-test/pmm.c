#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <mm/pmm.h>
#include <panic.h>
#include <ansii.h>
#include <drivers/serial/serial.h>

#define PMM_TEST_PAGES 8
static paddr_t pmm_pages[PMM_TEST_PAGES];

void pmm_test_self_test(void) {
    kprintf(LOG_INFO "Starting PMM Self-Test\n");

    for (size_t i = 0; i < PMM_TEST_PAGES; i++) {
        pmm_pages[i] = pmm_alloc_pages(1);
        if (!pmm_pages[i]) ke_panic("PMM Allocation failed");
        kprintf(LOG_INFO "PMM allocated page %zu paddr=%p\n", i, (void*)pmm_pages[i]);
    }

    for (size_t i = 0; i < PMM_TEST_PAGES; i++) {
        pmm_free_pages(pmm_pages[i], 1);
        kprintf(LOG_INFO "freed page %zu paddr=%p\n", i, (void*)pmm_pages[i]);
    }

    kprintf(LOG_OK "PMM Test Passed!\n");
}
