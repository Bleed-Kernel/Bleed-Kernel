#include <ACPI/acpi.h>
#include <ACPI/acpi_apic.h>
#include <ACPI/acpi_lapic.h>
#include <stddef.h>
#include <stdint.h>
#include <mm/paging.h>
#include <cpu/msrs.h>
#include <cpu/cpuid.h>
#include <panic.h>
#include <stdio.h>
#include <ansii.h>
#include <cpu/io.h>
#include "../acpi_priv.h"

#define IOAPIC_VIRT 0xFFFFFFFFFEC00000ULL

#define APIC_LVT_MASKED       (1U << 16)

#define IOAPIC_REG_SEL        0x00
#define IOAPIC_REG_WIN        0x10
#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VER        0x01
#define IOAPIC_REG_TABLE_BASE 0x10

static volatile uint32_t *ioapic = NULL;
static int apic_enabled = 0;

static inline void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic[IOAPIC_REG_SEL / 4] = reg;
    ioapic[IOAPIC_REG_WIN / 4] = val;
}

static inline uint32_t ioapic_read(uint32_t reg) {
    ioapic[IOAPIC_REG_SEL / 4] = reg;
    return ioapic[IOAPIC_REG_WIN / 4];
}

static void ioapic_write_redir(uint32_t pin, uint64_t entry) {
    uint32_t reg_low = IOAPIC_REG_TABLE_BASE + pin * 2;
    uint32_t reg_high = reg_low + 1;

    ioapic_write(reg_high, (uint32_t)(entry >> 32));
    ioapic_write(reg_low, (uint32_t)entry);
}

static void ioapic_mask_pin(uint32_t pin) {
    ioapic_write_redir(pin, APIC_LVT_MASKED);
}

int apic_is_enabled(void) {
    return apic_enabled;
}

static uint64_t ioapic_entry_for_irq(uint8_t vector, uint32_t lapic_id, uint32_t irq) {
    uint64_t entry = vector;
    uint16_t flags = acpi_irq_flags(irq);
    uint8_t polarity = flags & 0x3;
    uint8_t trigger = (flags >> 2) & 0x3;

    if (polarity == 0x3)
        entry |= (1ULL << 13);
    if (trigger == 0x3)
        entry |= (1ULL << 15);

    entry |= ((uint64_t)lapic_id << 56);
    return entry;
}

int apic_init(void) {
    uint64_t ioapic_phys = acpi_ioapic_base();
    if (!ioapic_phys)
        ke_panic("IOAPIC base not found");

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    inb(0x60);

    uint64_t ioapic_phys_page = ioapic_phys & PADDR_ENTRY_MASK;
    uintptr_t ioapic_off = (uintptr_t)(ioapic_phys & ~PADDR_ENTRY_MASK);
    paging_map_page(kernel_page_map, ioapic_phys_page, IOAPIC_VIRT,
                    PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    ioapic = (volatile uint32_t *)(IOAPIC_VIRT + ioapic_off);

    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint32_t max_redir = (ver >> 16) & 0xFF;

    for (uint32_t pin = 0; pin <= max_redir; pin++)
        ioapic_mask_pin(pin);

    uint32_t ioapic_gsi_base = acpi_ioapic_gsi_base();
    uint32_t bsp_id = lapic_get_id();

    struct {
        uint32_t irq;
        uint8_t vector;
    } irqs[] = {
        {1, 0x21},
        {12, 0x2C},
    };

    for (size_t i = 0; i < sizeof(irqs)/sizeof(irqs[0]); i++) {
        uint32_t gsi = acpi_irq_to_gsi(irqs[i].irq);
        if (gsi < ioapic_gsi_base)
            continue;

        uint32_t pin = gsi - ioapic_gsi_base;
        if (pin > max_redir)
            continue;

        uint64_t entry = ioapic_entry_for_irq(irqs[i].vector, bsp_id, irqs[i].irq);
        ioapic_write_redir(pin, entry);
    }

    apic_enabled = 1;
    return 0;
}