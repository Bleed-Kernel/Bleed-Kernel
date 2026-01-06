#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mm/pmm.h>
#include <panic.h>
#include <ansii.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>

#define PAGING_TEST_PAGES 4

static paddr_t phys_SEC1[PAGING_TEST_PAGES];
static paddr_t phys_SEC2[PAGING_TEST_PAGES];
static paddr_t SEC1, SEC2;

static void paging_test_allocate_test_pages() {
    kprintf(LOG_INFO "Allocating Physical Pages for Paging Test\n");
    for (size_t i = 0; i < PAGING_TEST_PAGES; i++) {
        phys_SEC1[i] = pmm_alloc_pages(1);
        phys_SEC2[i] = pmm_alloc_pages(1);

        void *v1 = paddr_to_vaddr(phys_SEC1[i]);
        void *v2 = paddr_to_vaddr(phys_SEC2[i]);
        if (!v1 || !v2) ke_panic("paddr_to_vaddr() Paging Allocation or Physical Address translation failed");

        memset(v1, 0, PAGE_SIZE);
        memset(v2, 0, PAGE_SIZE);

        kprintf(LOG_INFO "Allocated page %zu: SEC1 paddr=%p, SEC2 paddr=%p\n", i, (void*)phys_SEC1[i], (void*)phys_SEC2[i]);
    }
}

static void paging_test_map_and_init(paddr_t as, paddr_t *phys, uint64_t pattern) {
    kprintf(LOG_INFO "Paging: mapping and initializing address space\n");
    paging_switch_address_space(as);
    for (size_t i = 0; i < PAGING_TEST_PAGES; i++) {
        void *vaddr = paddr_to_vaddr(phys[i]);
        if (!vaddr) ke_panic("paddr_to_vaddr failed during mapping");

        *(volatile uint64_t*)vaddr = pattern | i;
        kprintf(LOG_INFO "Initialized page %zu: vaddr=%p, pattern=0x%llx\n", i, vaddr, pattern | i);
    }
}

static void paging_test_verify(paddr_t as, paddr_t *phys, uint64_t pattern) {
    kprintf(LOG_INFO "Paging: verifying address space\n");
    paging_switch_address_space(as);
    for (size_t i = 0; i < PAGING_TEST_PAGES; i++) {
        void *vaddr = paddr_to_vaddr(phys[i]);
        if (!vaddr) ke_panic("paddr_to_vaddr failed during verify");

        uint64_t val = *(volatile uint64_t*)vaddr;
        if (val != (pattern | i)) {
            kprintf(LOG_ERROR "Verification failed: vaddr=%p expected=0x%llx got=0x%llx\n", vaddr, pattern | i, val);
            ke_panic("Paging test mismatch");
        }
        kprintf(LOG_INFO "Verified page %zu: value=0x%llx\n", i, val);
    }
}

static void paging_test_cr3_stress_test() {
    kprintf(LOG_INFO "Paging: starting CR3 stress test (10000 switches)\n");
    for (size_t i = 0; i < 10000; i++) {
        paging_switch_address_space((i & 1) ? SEC1 : SEC2);
        if (i % 1000 == 0)
            kprintf(LOG_INFO "CR3 switch iteration %zu\n", i);
    }
}

static void paging_test_cleanup() {
    kprintf(LOG_INFO "Paging: cleaning up test address spaces and pages\n");
    paging_destroy_address_space(SEC1);
    paging_destroy_address_space(SEC2);
    for (size_t i = 0; i < PAGING_TEST_PAGES; i++) {
        pmm_free_pages(phys_SEC1[i], 1);
        pmm_free_pages(phys_SEC2[i], 1);
        kprintf(LOG_INFO "Freed page %zu: SEC1 paddr=%p, SEC2 paddr=%p\n", i, (void*)phys_SEC1[i], (void*)phys_SEC2[i]);
    }
}

void paging_test_self_test(void) {
    kprintf(LOG_INFO "Paging: self-test begin\n");

    SEC1 = paging_create_address_space();
    SEC2 = paging_create_address_space();
    if (!SEC1 || !SEC2) ke_panic("Failed to create test address spaces");
    kprintf(LOG_INFO "Created address spaces: SEC1=%p, SEC2=%p\n", (void*)SEC1, (void*)SEC2);

    paging_test_allocate_test_pages();
    paging_test_map_and_init(SEC1, phys_SEC1, 0xA1A1000000000000ULL);
    paging_test_map_and_init(SEC2, phys_SEC2, 0xB2B2000000000000ULL);

    paging_test_verify(SEC1, phys_SEC1, 0xA1A1000000000000ULL);
    paging_test_verify(SEC2, phys_SEC2, 0xB2B2000000000000ULL);

    volatile uint64_t *kptr = (uint64_t*)paddr_to_vaddr(kernel_page_map);
    uint64_t saved = *kptr;
    paging_switch_address_space(SEC2);
    if (*kptr != saved) ke_panic("Kernel mapping broken");
    paging_switch_address_space(kernel_page_map);
    kprintf(LOG_INFO "Kernel mapping verified\n");

    paging_test_cr3_stress_test();
    paging_test_cleanup();

    kprintf(LOG_OK "Paging Test Passed!\n");
}
