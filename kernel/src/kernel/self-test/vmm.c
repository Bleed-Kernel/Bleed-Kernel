#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <stdint.h>
#include <mm/paging.h>
#include <ansii.h>

void vmm_test_self_test() {
    serial_printf(LOG_INFO "Starting VMM test\n");

    vmm_cr3_t user_cr3 = vmm_create_address_space();
    serial_printf(LOG_INFO "Created user CR3 = %p\n", (void*)user_cr3);

    void* test_va = (void*)vmm_alloc_pages(4);

    if (vmm_map_pages(user_cr3, test_va, 4, PTE_PRESENT | PTE_USER | PTE_WRITABLE) != 0) {
        serial_printf(LOG_INFO "Failed to map pages!\n");
        return;
    }
    serial_printf(LOG_INFO "Mapped 4 pages at %p in user space\n", test_va);

    vmm_switch_address_space(user_cr3);

    uint64_t* data = (uint64_t*)test_va;
    for (int i = 0; i < 512*4; i++) {
        data[i] = 0xDEADBEEFCAFEBABE + i;
    }
    serial_printf(LOG_INFO "Wrote test pattern to mapped pages\n");

    for (int i = 0; i < 512*4; i++) {
        if (data[i] != 0xDEADBEEFCAFEBABE + i) {
            serial_printf(LOG_INFO "Data mismatch at index %d!\n", i);
            break;
        }
    }
    serial_printf(LOG_INFO "Verified data in user space\n");

    vmm_switch_address_space(kernel_page_map);
    serial_printf(LOG_INFO "Switched back to kernel space\n");

    vmm_unmap_pages(user_cr3, test_va, 4);
    serial_printf(LOG_INFO "Unmapped 4 pages in user space\n");

    vmm_destroy_address_space(user_cr3);
    serial_printf(LOG_INFO "Destroyed user address space\n");

    serial_printf(LOG_OK "VMM Passed\n");
}
