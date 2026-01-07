#include <stdint.h>
#include <mm/paging.h>
#include <stdio.h>
#include <panic.h>
#include "../acpi_priv.h"

__attribute__((noreturn))
void acpi_shutdown(void) {
    uint16_t cmd = ACPI_PM1_SLEEP_CMD(ACPI_S5);
    outw(PM1A_CNT, cmd);
    if (PM1B_CNT)
        outw(PM1B_CNT, cmd);
    for (;;)
        asm volatile("hlt");
}

__attribute__((noreturn))
void acpi_reboot(void) {
    if (!fadt)
        ke_panic("ACPI FADT not present");

    if (fadt->reset_reg_address == 0)
        ke_panic("ACPI reset register not present");

    if (fadt->reset_reg_address <= 0xFFFF) {
        outb((uint16_t)fadt->reset_reg_address, fadt->reset_value);
    } else {
        paddr_t phys_page = fadt->reset_reg_address & ~(PAGE_SIZE_4K - 1);
        size_t offset = fadt->reset_reg_address & (PAGE_SIZE_4K - 1);

        void *vpage = NULL;
        paging_alloc_empty_frame(&vpage);
        if (!vpage)
            ke_panic("Failed to allocate virtual page for ACPI reset MMIO");

        paging_map_page(read_cr3(), phys_page, (paddr_t)vpage, PAGE_KERNEL_RW);

        volatile uint8_t *reset_mmio = (volatile uint8_t *)((uintptr_t)vpage + offset);
        *reset_mmio = fadt->reset_value;
    }

    kprintf("ACPI Reboot attempted: Reset Register Address: %p, Reset Value: %u\n", 
            (void *)fadt->reset_reg_address, fadt->reset_value);
    for (;;)
        __asm__("hlt");
}