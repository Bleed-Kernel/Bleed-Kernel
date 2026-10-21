#include <stdint.h>
#include <stdbool.h>
#include <cpu/io.h>
#include <mm/paging.h>
#include <stdio.h>
#include <kernel/exception/panic.h>
#include <ansii.h>
#include <drivers/serial/serial.h>
#include <console/console.h>
#include <devices/type/tty_device.h>
#include <mm/pmm.h>
#include <cpu/control_registers.h>
#include "../acpi_priv.h"

#define FADT_FLAG_RESET_REG_SUP (1u << 10)
#define PM1_PWRBTN_STS          (1u << 8)

void acpi_poweroff_fallback(){
    // ideally we never get here, but if we do so be it
    // at this point all services should be stopped and any physical disks should not be spinning
    // and the computer is ready to power off, but ACPI has failed us so we should
    // inform the user they can do it manually.
    // its also a cute nod to history from the windows 9x era
    tty_t tty = console_get_active_tty();
    kprintf("\x1b[2J");

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
    kprintf(RESET);
}

// the PM1 event registers can be split over two blocks, the real register is both of them
// together. most machines only have the first and give 0 for the second
#define PM1_BLOCKS 2

static uint32_t acpi_pm1_block(int i) {
    return i == 0 ? fadt->pm1a_evt_blk : fadt->pm1b_evt_blk;
}

// 1 if bit was set in the status half of either block. written back as a 1 for clear
static int acpi_pm1_take(uint16_t bit) {
    int taken = 0;

    for (int i = 0; i < PM1_BLOCKS; i++) {
        uint16_t status = (uint16_t)acpi_pm1_block(i);
        if (!status || !(inw(status) & bit)) continue;

        outw(status, bit);
        taken = 1;
    }
    return taken;
}

// the enable half sits after the status half
static void acpi_pm1_enable(uint16_t bit) {
    if (fadt->pm1_evt_len < 4) return;

    for (int i = 0; i < PM1_BLOCKS; i++) {
        uint32_t block = acpi_pm1_block(i);
        if (!block) continue;

        uint16_t enable = (uint16_t)(block + fadt->pm1_evt_len / 2);
        outw(enable, inw(enable) | bit);
    }
}

// the press is latched in PM1 status. some machines (qemu is one) only latch it once the event
// is enabled, the interrupt that comes with that is never unmasked
void acpi_power_button_enable(void) {
    if (!fadt) return;

    acpi_pm1_enable(PM1_PWRBTN_STS);

    // whatever turning the machine on left latched
    acpi_pm1_take(PM1_PWRBTN_STS);
}

int acpi_power_button_pressed(void) {
    return fadt ? acpi_pm1_take(PM1_PWRBTN_STS) : 0;
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
    uint64_t reset_address = 0;
    uint8_t reset_value = 0;
    uint8_t reset_space = 0xFF;
    bool acpi_reset_valid = false;

    if (!fadt)
        ke_panic(NULL, "ACPI FADT not present");

    // harden ACPI reset so we only try to perform one if its valid
    size_t reset_reg_end = offsetof(struct acpi_fadt, reset_reg) + sizeof(fadt->reset_reg);
    size_t reset_value_end = offsetof(struct acpi_fadt, reset_value) + sizeof(fadt->reset_value);
    bool has_reset_fields = (fadt->header.length >= reset_reg_end) && (fadt->header.length >= reset_value_end);
    bool reset_flag_set = (fadt->flags & FADT_FLAG_RESET_REG_SUP) != 0;

    if (has_reset_fields) {
        reset_address = fadt->reset_reg.address;
        reset_value = fadt->reset_value;
        reset_space = fadt->reset_reg.address_space_id;

        if (reset_flag_set && reset_address != 0 && (reset_space == 0 || reset_space == 1))
            acpi_reset_valid = true;
    }

    asm volatile ("cli");
    kprintf(
        "%sACPI reboot: fadt_len=%u reset_supported=%u reset_reg_present=%u reset_addr_space=%u reset_addr=%p reset_value=%u\n",
        LOG_INFO,
        fadt->header.length,
        (unsigned)reset_flag_set,
        (unsigned)has_reset_fields,
        (unsigned)reset_space,
        (void *)reset_address,
        reset_value
    );

    if (acpi_reset_valid) {
        if (reset_space == 1) {
            outb((uint16_t)reset_address, reset_value);
        } else {
            // reach the register through the hhdm, if it wont map the 0xCF9 fallback below still runs
            if (paging_map_mmio((paddr_t)reset_address, 1) == 0) {
                volatile uint8_t *reset_mmio = (volatile uint8_t *)paddr_to_vaddr((paddr_t)reset_address);
                *reset_mmio = reset_value;
            }
        }
    }

    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);

    for (uint32_t i = 0; i < 100000; i++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
    }
    outb(0x64, 0xFE);

    serial_printf("%sACPI reboot failed to reset hardware; entering halted state", LOG_ERROR);
    kprintf("\n");

    acpi_poweroff_fallback();

    for (;;)
        __asm__("hlt");
}