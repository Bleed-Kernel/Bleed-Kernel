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
#include <ACPI/acpi_hpet.h>
#include "../acpi_priv.h"

static volatile uint32_t *lapic = NULL;
static uint32_t lapic_ticks_per_10ms = 0;

void calibrate_wait_ms(uint64_t ms) {
    uint64_t ticks = (ms * femtosecondsPerMillisecond) / getFemtosecondsPerTick();
    uint64_t start = hpet_read_counter();

    while (hpet_read_counter() - start < ticks) {
        asm volatile("pause");
    }
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
    (void)lapic[0];
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

void apic_eoi(void) {
    APIC_EOI();
}

static int cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    return (edx & (1 << 9)) != 0;
}

uint32_t lapic_get_id(void) {
    if (!lapic) return 0;
    return lapic[LAPIC_REG_ID / 4] >> 24;
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_REG_LVT_TIMER, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_INIT_CNT, 0);
}

void lapic_timer_calibrate(void) {
    lapic_timer_stop();
    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIVIDE_16);

    lapic_write(LAPIC_REG_LVT_TIMER, 0xFF);
    lapic_write(LAPIC_REG_TIMER_INIT_CNT, 0xFFFFFFFF);

    calibrate_wait_ms(10);

    lapic_write(LAPIC_REG_LVT_TIMER, APIC_LVT_MASKED);
    uint32_t ticks_passed = 0xFFFFFFFF - lapic_read(LAPIC_REG_TIMER_CURR_CNT);
    
    lapic_ticks_per_10ms = ticks_passed;
}

void lapic_timer_start(uint8_t vector) {
    lapic_write(LAPIC_REG_TIMER_DIV, TIMER_DIVIDE_16);
    lapic_write(LAPIC_REG_LVT_TIMER, TIMER_PERIODIC | vector);
    
    lapic_write(LAPIC_REG_TIMER_INIT_CNT, lapic_ticks_per_10ms);
}

int lapic_init(void) {
    if (!cpu_has_apic())
        ke_panic("Bleed Requires APIC support");

    uint64_t lapic_phys = acpi_lapic_base();
    if (!lapic_phys)
        ke_panic("LAPIC timer base not found");

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

    uint32_t lapic_id = lapic_get_id();
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

    lapic_timer_calibrate();
    hpet_stop_timer0_interrupts();
    lapic_timer_start(0x20);

    APIC_EOI();

    return 0;
}