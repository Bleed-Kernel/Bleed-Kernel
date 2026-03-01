#include <ACPI/acpi.h>
#include <ACPI/acpi_apic.h>
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

#define LAPIC_SIZE  0x1000
#define LAPIC_VIRT  0xFFFFFFFFFFE00000ULL
#define IOAPIC_VIRT 0xFFFFFFFFFEC00000ULL

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_ENABLE (1ULL << 11)

#define LAPIC_REG_ID          0x020
#define LAPIC_REG_EOI         0x0B0
#define LAPIC_REG_SVR         0x0F0
#define LAPIC_REG_TPR         0x080
#define LAPIC_REG_DFR         0x0E0
#define LAPIC_REG_LDR         0x0D0
#define LAPIC_REG_LVT_TIMER   0x320
#define LAPIC_REG_LVT_THERMAL 0x330
#define LAPIC_REG_LVT_PMC     0x340
#define LAPIC_REG_LVT_LINT0   0x350
#define LAPIC_REG_LVT_LINT1   0x360
#define LAPIC_REG_LVT_ERROR   0x370

#define APIC_LVT_MASKED       (1U << 16)
#define APIC_SVR_ENABLE       (1U << 8)

#define IOAPIC_REG_SEL        0x00
#define IOAPIC_REG_WIN        0x10
#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VER        0x01
#define IOAPIC_REG_TABLE_BASE 0x10

static volatile uint32_t *lapic = NULL;
static volatile uint32_t *ioapic = NULL;
static int apic_enabled = 0;

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
    (void)lapic[0];
}

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

    // dest first then unmask/control
    ioapic_write(reg_high, (uint32_t)(entry >> 32));
    ioapic_write(reg_low, (uint32_t)entry);
}

static void ioapic_mask_pin(uint32_t pin) {
    ioapic_write_redir(pin, APIC_LVT_MASKED);
}

int apic_is_enabled(void) {
    return apic_enabled;
}

// this is here beacuse it kinda sucks for the
// assembly. i might end up using it in C so ill keep the macro
void apic_eoi(void) {
    APIC_EOI();
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

static int cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    return (edx & (1 << 9)) != 0;
}

int apic_init(void) {
    if (!cpu_has_apic())
        ke_panic("Bleed Requires APIC support");

    uint64_t lapic_phys = acpi_lapic_base();
    if (!lapic_phys)
        ke_panic("LAPIC timer base not found");
        
    uint64_t ioapic_phys = acpi_ioapic_base();
    if (!ioapic_phys)
        ke_panic("IOAPIC base not found");

    uint64_t lapic_phys_page = lapic_phys & PADDR_ENTRY_MASK;
    uintptr_t lapic_off = (uintptr_t)(lapic_phys & ~PADDR_ENTRY_MASK);
    paging_map_page(kernel_page_map, lapic_phys_page, LAPIC_VIRT,
                    PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    lapic = (volatile uint32_t *)(LAPIC_VIRT + lapic_off);

    uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
    if (!(apic_msr & IA32_APIC_BASE_ENABLE))
        ke_panic("APIC: LAPIC disabled");

    apic_msr &= 0xFFFULL;
    apic_msr |= lapic_phys_page;
    apic_msr |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, apic_msr);

    uint32_t lapic_id = lapic[LAPIC_REG_ID / 4] >> 24;
    kprintf(LOG_INFO "LAPIC ID: %u\n", lapic_id);

    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_REG_LDR, (1U << (lapic_id & 0xF)) << 24);
    lapic_write(LAPIC_REG_LVT_THERMAL, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_PMC, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT0, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_TIMER, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_SVR, APIC_SVR_ENABLE | 0x2F);
    lapic_write(LAPIC_REG_EOI, 0);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

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

    uint32_t bsp_id = lapic_id;
    struct {
        uint32_t irq;
        uint8_t vector;
    } irqs[] = {
        {0, 0x20},
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
