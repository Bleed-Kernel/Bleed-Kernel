#include <ACPI/acpi.h>
#include <ACPI/acpi_hpet.h>
#include <stdint.h>
#include <stddef.h>
#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <ansii.h>
#include <panic.h>
#include <sched/scheduler.h>

#define HPET_FREQUENCY          1000

#define HPET_TIMER0_CONFIG      0x100
#define HPET_TIMER_INTERRUPTS   (1U << 2)

struct acpi_hpet *hpet = NULL;
void* address = NULL;

static uint64_t femtosecondsPerTick = 0;
static uint64_t millisecondsPerTick = 0;

uint64_t getFemtosecondsPerTick() { return femtosecondsPerTick; }
uint64_t getMillisecondsPerTick() { return millisecondsPerTick; }
void* getHpetAddress() { return address; }

void hpet_stop_timer0_interrupts(void) {
    if (!address) return;

    uint32_t config = *(volatile uint32_t*)(address + HPET_TIMER0_CONFIG);
    config &= ~HPET_TIMER_INTERRUPTS;
    *(volatile uint32_t*)(address + HPET_TIMER0_CONFIG) = config;
}

void acpi_init_hpet(void){
    hpet = (struct acpi_hpet *)acpi_find_sdt("HPET");
    if (!hpet) {
        serial_printf(LOG_ERROR "HPET Table not found\n");
        return;
    } else {
        serial_printf(LOG_OK "HPET Found\n");
    }

    address = paddr_to_vaddr(hpet->address.address);

    paging_map_page(kernel_page_map, (uint64_t)hpet->address.address, (uint64_t)address, PTE_PRESENT | PTE_WRITABLE);
    
    *(uint32_t*)(address + 0x10)    |= HPET_MAINCOUNTER_ENABLE  |   HPET_LEGACY_REPLACEMENT;
    *(uint32_t*)(address + 0x100)   |= HPET_TIMER_INTERUPTS     |   HPET_TIMER_PERIODIC;

    femtosecondsPerTick = *(uint64_t*)address >> 32;
    millisecondsPerTick = femtosecondsPerMillisecond / femtosecondsPerTick;
    serial_printf(LOG_OK "HPET is at %u femtoseconds per tick\n", femtosecondsPerTick);

    *(uint64_t*)(address + 0x108) = (femtosecondsPerSecond / femtosecondsPerTick) / HPET_FREQUENCY;
    *(uint64_t*)(address + 0xF0) = 0;
}

void wait_fs(uint64_t femtoseconds) {
    if (femtosecondsPerTick == 0)
        return;

    uint64_t ticks = femtoseconds / femtosecondsPerTick;
    if (ticks == 0)
        ticks = 1;

    uint64_t start  = hpet_read_counter();
    uint64_t target = start + ticks;

    while (hpet_read_counter() < target) {
        sched_yield(get_current_task());
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
    if (!address || femtosecondsPerTick == 0)
        return 0;
    return *(uint64_t*)(address + 0xF0) * femtosecondsPerTick;
}