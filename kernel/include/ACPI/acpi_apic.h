#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mm/paging.h>
#include <ACPI/acpi.h>

#define LAPIC_SIZE        0x1000
#define LAPIC_VIRT        0xFFFFFFFFFFE00000ULL
#define IOAPIC_VIRT       0xFFFFFFFFFEC00000ULL

#define IA32_APIC_BASE_MSR     0x1B
#define IA32_APIC_BASE_ENABLE  (1ULL << 11)

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

#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VER        0x01
#define IOAPIC_REG_TABLE_BASE 0x10

#define APIC_EOI() \
    do { \
        if (apic_is_enabled() && lapic) \
            lapic[LAPIC_REG_EOI / 4] = 0; \
    } while(0)

int apic_init(void);
int apic_is_enabled(void);
static uint64_t ioapic_entry_for_irq(uint8_t vector, uint32_t lapic_id, uint32_t irq);