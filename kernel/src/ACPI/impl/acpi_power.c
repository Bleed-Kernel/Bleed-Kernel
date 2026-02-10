#include <stdint.h>
#include <mm/paging.h>
#include <stdio.h>
#include <panic.h>
#include <ansii.h>
#include <drivers/serial/serial.h>
#include <console/console.h>
#include <devices/type/tty_device.h>
#include <panic.h>
#include <mm/pmm.h>
#include <cpu/control_registers.h>
#include "../acpi_priv.h"

void acpi_poweroff_fallback(){
    // ideally we never get here, but if we do so be it
    // at this point all services should be stopped and any physical disks should not be spinning
    // and the computer is ready to power off, but ACPI has failed us so we should
    // inform the user they can do it manually.
    // its also a cute nod to history from the windows 9x era
    tty_t tty = console_get_active_tty();
    kprintf("\x1b[J");

    tty_fb_backend_t *b = tty.backend;
    fb_console_t *fb = &b->fb;

    uint64_t cols = fb->width  / fb->font->width;
    uint64_t rows = fb->height / fb->font->height;

    const char *line1 = "It is now safe to turn off";
    const char *line2 = "your computer";

    size_t len1 = strlen(line1);
    size_t len2 = strlen(line2);

    uint64_t x1 = (cols > len1) ? (cols - len1) / 2 : 0;
    uint64_t x2 = (cols > len2) ? (cols - len2) / 2 : 0;

    uint64_t y = (rows / 2) - 1;

    kprintf(ORANGE_FG);

    kprintf_pos(x1, y,     "%s", line1);
    kprintf_pos(x2, y + 1, "%s", line2);
}

static uint16_t SLP_TYPa = 0;
static uint16_t SLP_TYPb = 0;

void acpi_parse_s5(void) {
    if (!fadt || !fadt->dsdt) return;

    char *dsdt = (char *)paddr_to_vaddr((uintptr_t)fadt->dsdt);

    if (memcmp(dsdt, "DSDT", 4) != 0) return;

    uint32_t dsdt_len = *(uint32_t*)(dsdt + 4);
    char *ptr = dsdt + 36;
    char *end = dsdt + dsdt_len;

    while (ptr < end) {
        if (memcmp(ptr, "_S5_", 4) == 0) {
            ptr += 4;
            if (*ptr == 0x12) {
                ptr += 3;
                
                if (*ptr == 0x0A) ptr++; 
                SLP_TYPa = (*(uint8_t*)ptr) << 10;
                ptr++;

                if (*ptr == 0x0A) ptr++;
                SLP_TYPb = (*(uint8_t*)ptr) << 10;
                
                return;
            }
        }
        ptr++;
    }
}

__attribute__((noreturn))
void acpi_shutdown(void) {
    if (SLP_TYPa == 0) acpi_parse_s5();
    if (SLP_TYPa == 0) SLP_TYPa = (5 << 10); 

    outw((uint16_t)fadt->pm1a_cnt_blk, SLP_TYPa | SLP_EN);
    
    if (fadt->pm1b_cnt_blk != 0) {
        outw((uint16_t)fadt->pm1b_cnt_blk, SLP_TYPb | SLP_EN);
    }

    //virtulizers last just incase these have some sort of unintended effect on real hw
    outw(0xB004, 0x2000);   // bochs
    outw(0x4004, 0x3400);   // vbox
    outw(0x604, 0x2000);    // QEMU
    outw(0x600, 0x34);      // Cloud Hypervisor

    acpi_poweroff_fallback();
    asm volatile("cli");
    for(;;) asm volatile("hlt");
}

__attribute__((noreturn))
void acpi_reboot(void) {
    asm volatile ("cli");
    kprintf(
        "%sACPI Reboot attempted: Reset Register Address: %p, Reset Value: %u\nOffsets:\n\treset_reg: %ld\n\treset_value: %ld\n",
        LOG_INFO,
        (void *)fadt->reset_reg.address,
        fadt->reset_value,
        offsetof(struct acpi_fadt, reset_reg),
        offsetof(struct acpi_fadt, reset_value)
    );

    if (!fadt)
        ke_panic("ACPI FADT not present");

    if (fadt->reset_reg.address == 0)
        ke_panic("ACPI reset register not present");

    if (fadt->reset_reg.address <= 0xFFFF) {
        outb((uint16_t)fadt->reset_reg.address, fadt->reset_value);
    } else {
        paddr_t phys_page = fadt->reset_reg.address & ~(PAGE_SIZE_4K - 1);
        size_t offset = fadt->reset_reg.address & (PAGE_SIZE_4K - 1);

        void *vpage = NULL;
        paging_alloc_empty_frame(&vpage);
        if (!vpage)
            ke_panic("Failed to allocate virtual page for ACPI reset MMIO");

        paging_map_page(read_cr3(), phys_page, (paddr_t)vpage, PAGE_KERNEL_RW);

        volatile uint8_t *reset_mmio = (volatile uint8_t *)((uintptr_t)vpage + offset);
        *reset_mmio = fadt->reset_value;
    }


    serial_printf("%sACPI Power Control Failed to initiate shutdown, despite this all services are stopped", LOG_ERROR);
    kprintf("\n");

    serial_printf(
        RESET "ACPI Reboot attempted: Reset Register Address: %p, Reset Value: %u\n",
        (void *)fadt->reset_reg.address,
        fadt->reset_value
    );

    acpi_poweroff_fallback();

    for (;;)
        __asm__("hlt");
}