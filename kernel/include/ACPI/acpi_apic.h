#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mm/paging.h>
#include <ACPI/acpi.h>

#define IOAPIC_VIRT       0xFFFFFFFFFEC00000ULL

#define IA32_APIC_BASE_MSR     0x1B
#define IA32_APIC_BASE_ENABLE  (1ULL << 11)
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