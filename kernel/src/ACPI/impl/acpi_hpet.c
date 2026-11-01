#include <ACPI/acpi.h>
#include <ACPI/acpi_hpet.h>
#include <ACPI/acpi_apic.h>
#include <stdint.h>
#include <stddef.h>
#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <ansii.h>
#include <kernel/exception/panic.h>
#include <sched/scheduler.h>
#include <mm/spinlock.h>
#include <cpu/irq.h>

#define HPET_FREQUENCY          1000

#define HPET_REG_CAPABILITIES   0x000
#define HPET_REG_CONFIG         0x010
#define HPET_REG_MAIN_COUNTER   0x0F0
#define HPET_REG_T0_CONFIG      0x100
#define HPET_REG_T0_COMPARATOR  0x108

#define HPET_CAP_COUNTER_64     (1ULL << 13)
#define HPET_COUNTER_WRAP       (1ULL << 32)

// the tick reads it this often so a quiet machine cant sleep through a wrap
#define HPET_KEEPUP_TICKS       60000

#define RFLAGS_IF               0x200ULL

struct acpi_hpet *hpet = NULL;
static volatile uint8_t *hpet_base = NULL;

uint64_t femtosecondsPerTick = 0;

// amd chipsets only have 32 bits of counter, gone round in 5 minutes. bits above that are ours
static bool hpet_counter_32 = false;
static volatile uint64_t hpet_extended = 0;

static inline uint64_t hpet_read64(size_t off) {
    return *(volatile uint64_t *)(hpet_base + off);
}

static inline void hpet_write64(size_t off, uint64_t v) {
    *(volatile uint64_t *)(hpet_base + off) = v;
}

static void hpet_keepup(void *ctx) {
    static uint64_t ticks;
    (void)ctx;

    if (++ticks >= HPET_KEEPUP_TICKS) {
        ticks = 0;
        hpet_read_counter();
    }
}

void acpi_init_hpet(void){
    hpet = (struct acpi_hpet *)acpi_find_sdt("HPET");
    if (!hpet) {
        serial_printf(LOG_ERROR "HPET Table not found\n");
        return;
    } else {
        serial_printf(LOG_OK "HPET Found\n");
    }

    // own vaddr like the lapic, the hhdm one sits inside a limine huge page
    uint64_t  hpet_phys_page = hpet->address.address & PADDR_ENTRY_MASK;
    uintptr_t hpet_off       = (uintptr_t)(hpet->address.address & ~PADDR_ENTRY_MASK);
    paging_map_page(kernel_page_map, hpet_phys_page, HPET_VIRT,
                    PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    hpet_base = (volatile uint8_t *)(HPET_VIRT + hpet_off);

    uint64_t caps = hpet_read64(HPET_REG_CAPABILITIES);
    femtosecondsPerTick = caps >> 32;
    hpet_counter_32 = !(caps & HPET_CAP_COUNTER_64);
    serial_printf(LOG_OK "HPET is at %u femtoseconds per tick, %u bit counter\n", femtosecondsPerTick,
                  hpet_counter_32 ? 32 : 64);

    hpet_write64(HPET_REG_CONFIG, hpet_read64(HPET_REG_CONFIG) & ~(uint64_t)(HPET_MAINCOUNTER_ENABLE | HPET_LEGACY_REPLACEMENT));
    hpet_write64(HPET_REG_MAIN_COUNTER, 0);
    hpet_write64(HPET_REG_T0_CONFIG, hpet_read64(HPET_REG_T0_CONFIG) & ~(uint64_t)HPET_TIMER_INTERRUPTS);
    hpet_write64(HPET_REG_CONFIG, hpet_read64(HPET_REG_CONFIG) | HPET_MAINCOUNTER_ENABLE);

    if (hpet_counter_32)
        tick_register(hpet_keepup, NULL);

    // not all manufacturers will actually allow you to tick off the HPET
    if (lapic_timer_start(HPET_FREQUENCY) == 0)
        return;

    serial_printf(LOG_WARN "LAPIC timer unavailable, ticking off HPET timer 0\n");

    hpet_write64(HPET_REG_CONFIG, hpet_read64(HPET_REG_CONFIG) & ~(uint64_t)HPET_MAINCOUNTER_ENABLE);
    hpet_write64(HPET_REG_MAIN_COUNTER, 0);
    hpet_extended = 0;

    hpet_write64(HPET_REG_T0_CONFIG, hpet_read64(HPET_REG_T0_CONFIG) | HPET_TIMER_INTERRUPTS | HPET_TIMER_PERIODIC | HPET_TIMER_VAL_SET);
    hpet_write64(HPET_REG_T0_COMPARATOR, (femtosecondsPerSecond / femtosecondsPerTick) / HPET_FREQUENCY);

    hpet_write64(HPET_REG_CONFIG, hpet_read64(HPET_REG_CONFIG) | HPET_MAINCOUNTER_ENABLE | HPET_LEGACY_REPLACEMENT);
}

void wait_fs(uint64_t femtoseconds) {
    if (femtosecondsPerTick == 0)
        return;

    uint64_t ticks = femtoseconds / femtosecondsPerTick;
    if (ticks == 0)
        ticks = 1;

    uint64_t start  = hpet_read_counter();
    uint64_t target = start + ticks;

    // short waits spin: they are over before a switch would be, and drivers do them with a
    // lock held or before theres a scheduler at all. only a long one is worth giving the cpu up for
    int can_yield = femtoseconds >= femtosecondsPerMillisecond && get_current_task() != NULL;

    if (can_yield) {
        unsigned long flags = irq_push();
        if (flags & RFLAGS_IF) {
            while (hpet_read_counter() < target)
                sched_sleep_until(target);
            irq_restore(flags);
            return;
        }
        irq_restore(flags);
    }

    while (hpet_read_counter() < target) {
        if (can_yield)
            sched_yield(get_current_task());
        else
            asm volatile("pause");
    }
}

void wait_us(uint64_t us) {
    wait_fs(us * femtosecondsPerMicrosecond);
}

void wait_ms(uint64_t ms) {
    wait_fs(ms * femtosecondsPerMillisecond);
}

void wait_s(uint64_t s) {
    wait_fs(s * femtosecondsPerSecond);
}

void wait_ns(uint64_t ns) {
    wait_fs(ns * femtosecondsPerNanosecond);
}

uint64_t hpet_get_femtoseconds(){
    if (!hpet_base || femtosecondsPerTick == 0)
        return 0;
    return hpet_read_counter() * femtosecondsPerTick;
}

uint64_t hpet_read_counter(void) {
    uint64_t raw = hpet_read64(HPET_REG_MAIN_COUNTER);
    if (!hpet_counter_32)
        return raw;

    raw &= HPET_COUNTER_WRAP - 1;
    uint64_t last = __atomic_load_n(&hpet_extended, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t now = (last & ~(HPET_COUNTER_WRAP - 1)) | raw;

        // a little behind means another cpu read it after we did
        if (now < last) {
            if (last - now < HPET_COUNTER_WRAP / 2)
                return last;
            now += HPET_COUNTER_WRAP;
        }
        if (__atomic_compare_exchange_n(&hpet_extended, &last, now, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return now;
    }
}
bool hpet_timed_out(uint64_t start, uint64_t spins, uint64_t timeout_ms) {
    uint64_t now = hpet_get_femtoseconds();
    if (now)
        return now - start > timeout_ms * femtosecondsPerMillisecond;
    return spins > timeout_ms * 1000;
}

void hpet_busy_wait_ms(uint64_t ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; !hpet_timed_out(start, spins, ms); spins++)
        asm volatile("pause");
}
