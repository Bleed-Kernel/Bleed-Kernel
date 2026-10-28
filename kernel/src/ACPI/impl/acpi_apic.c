#include <mm/spinlock.h>
#include <ACPI/acpi.h>
#include <ACPI/acpi_apic.h>
#include <stddef.h>
#include <stdint.h>
#include <mm/paging.h>
#include <cpu/msrs.h>
#include <cpu/cpuid.h>
#include <kernel/exception/panic.h>
#include <stdio.h>
#include <ansii.h>
#include <cpu/io.h>
#include <ACPI/acpi_hpet.h>
#include <drivers/serial/serial.h>
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

#define LAPIC_REG_ICR_LOW     0x300
#define LAPIC_REG_ICR_HIGH    0x310
#define LAPIC_ICR_NMI         (4U << 8)
#define LAPIC_ICR_PENDING     (1U << 12)
#define LAPIC_ICR_ASSERT      (1U << 14)
#define LAPIC_ICR_ALL_BUT_SELF (3U << 18)
#define LAPIC_REG_TIMER_INIT  0x380
#define LAPIC_REG_TIMER_CUR   0x390
#define LAPIC_REG_TIMER_DIV   0x3E0

#define LAPIC_TIMER_PERIODIC  (1U << 17)
#define LAPIC_TIMER_DIV_16    0x3
#define LAPIC_TIMER_VECTOR    0x20
#define LAPIC_CALIBRATE_MS    10

#define IOAPIC_REG_SEL        0x00
#define IOAPIC_REG_WIN        0x10
#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VER        0x01
#define IOAPIC_REG_TABLE_BASE 0x10

static volatile uint32_t *lapic = NULL;
static volatile uint32_t *ioapic = NULL;
static int apic_enabled = 0;
static uint32_t ioapic_max_redir = 0;
static uint32_t bsp_lapic_id = 0;
static uint32_t lapic_tick_count = 0;   // what the bsp calibrated, the aps reuse it

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

static void lapic_icr_wait(void) {
    while (lapic[LAPIC_REG_ICR_LOW / 4] & LAPIC_ICR_PENDING)
        asm volatile("pause");
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    unsigned long flags = irq_push();
    lapic_icr_wait();
    lapic_write(LAPIC_REG_ICR_HIGH, lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, vector | LAPIC_ICR_ASSERT);
    lapic_icr_wait();
    irq_restore(flags);
}

void lapic_nmi_others(void) {
    unsigned long flags = irq_push();
    lapic_icr_wait();
    lapic_write(LAPIC_REG_ICR_HIGH, 0);
    lapic_write(LAPIC_REG_ICR_LOW, LAPIC_ICR_NMI | LAPIC_ICR_ASSERT | LAPIC_ICR_ALL_BUT_SELF);
    lapic_icr_wait();
    irq_restore(flags);
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

// -1 if the gsi belongs to some other ioapic, theres only the one in here
static int ioapic_pin_for_gsi(uint32_t gsi) {
    uint32_t base = acpi_ioapic_gsi_base();
    if (!apic_enabled || gsi < base || gsi - base > ioapic_max_redir)
        return -1;
    return (int)(gsi - base);
}

int ioapic_route_gsi(uint32_t gsi, uint8_t vector, int level, int low) {
    int pin = ioapic_pin_for_gsi(gsi);
    if (pin < 0) return -1;

    uint64_t entry = vector;
    if (low)
        entry |= (1ULL << 13);
    if (level)
        entry |= (1ULL << 15);
    entry |= ((uint64_t)bsp_lapic_id << 56);

    ioapic_write_redir((uint32_t)pin, entry);
    return 0;
}

void ioapic_mask_gsi(uint32_t gsi) {
    int pin = ioapic_pin_for_gsi(gsi);
    if (pin >= 0)
        ioapic_mask_pin((uint32_t)pin);
}

/// @brief start the LAPIC timer as the periodic scheduler tick on vector 0x20
/// @param hz tick rate
/// @return 0 on success, -1 if it couldnt be calibrated (no LAPIC or no running HPET)
int lapic_timer_start(uint32_t hz) {
    if (!apic_enabled || !lapic || !femtosecondsPerTick || !hz)
        return -1;

    uint64_t hpet_ticks = (LAPIC_CALIBRATE_MS * femtosecondsPerMillisecond) / femtosecondsPerTick;

    lapic_write(LAPIC_REG_LVT_TIMER, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, 0xFFFFFFFF);

    // calibrate lapic timere
    uint64_t start = hpet_read_counter();
    uint64_t spins = 0;
    while (hpet_read_counter() - start < hpet_ticks) {
        // fuck
        if (++spins > 100000000ULL) {
            lapic_write(LAPIC_REG_TIMER_INIT, 0);
            return -1;
        }
        asm volatile("pause");
    }

    uint32_t elapsed = 0xFFFFFFFF - lapic[LAPIC_REG_TIMER_CUR / 4];
    lapic_write(LAPIC_REG_TIMER_INIT, 0);

    uint64_t per_tick = ((uint64_t)elapsed * 1000) / ((uint64_t)LAPIC_CALIBRATE_MS * hz);
    if (!elapsed || !per_tick || per_tick > 0xFFFFFFFF)
        return -1;

    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, (uint32_t)per_tick);
    lapic_tick_count = (uint32_t)per_tick;

    serial_printf(LOG_OK "LAPIC timer ticking at %u Hz (%u counts per tick)\n", hz, (uint32_t)per_tick);
    return 0;
}

/// @brief set up the calling aps lapic the way apic_init did the bsps and start its tick
/// @return 0 on success, -1 if the bsp never got a timer going to copy
int lapic_ap_init(void) {
    if (!apic_enabled || !lapic || !lapic_tick_count)
        return -1;

    // the mapping is shared, every cpu sees its own lapic behind it
    wrmsr(IA32_APIC_BASE_MSR, rdmsr(IA32_APIC_BASE_MSR) | IA32_APIC_BASE_ENABLE);
    uint32_t lapic_id = lapic[LAPIC_REG_ID / 4] >> 24;

    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_REG_LDR, (1U << (lapic_id & 0xF)) << 24);
    lapic_write(LAPIC_REG_LVT_THERMAL, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_PMC, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT0, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_LINT1, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_LVT_ERROR, APIC_LVT_MASKED);
    lapic_write(LAPIC_REG_SVR, APIC_SVR_ENABLE | 0x2F);
    lapic_write(LAPIC_REG_EOI, 0);

    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_DIV, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_REG_TIMER_INIT, lapic_tick_count);
    return 0;
}

static int cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    if (!cpuid(1, &eax, &ebx, &ecx, &edx))
        return 0;
    return (edx & (1 << 9)) != 0;
}

int apic_init(void) {
    if (!cpu_has_apic())
        ke_panic(NULL, "Bleed Requires APIC support");

    uint64_t lapic_phys = acpi_lapic_base();
    if (!lapic_phys)
        ke_panic(NULL, "LAPIC timer base not found");
        
    uint64_t ioapic_phys = acpi_ioapic_base();
    if (!ioapic_phys)
        ke_panic(NULL, "IOAPIC base not found");

    uint64_t lapic_phys_page = lapic_phys & PADDR_ENTRY_MASK;
    uintptr_t lapic_off = (uintptr_t)(lapic_phys & ~PADDR_ENTRY_MASK);
    paging_map_page(kernel_page_map, lapic_phys_page, LAPIC_VIRT,
                    PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    lapic = (volatile uint32_t *)(LAPIC_VIRT + lapic_off);

    uint64_t apic_msr = rdmsr(IA32_APIC_BASE_MSR);
    if (!(apic_msr & IA32_APIC_BASE_ENABLE))
        ke_panic(NULL, "APIC: LAPIC disabled");

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
    ioapic_max_redir = max_redir;
    bsp_lapic_id     = lapic_id;
    struct {
        uint32_t irq;
        uint8_t vector;
    } irqs[] = {
        {0, 0x20},
        {1, 0x21},
        {4, 0x24},
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
