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

struct acpi_hpet *hpet = NULL;
volatile uint64_t *address = NULL;

uint64_t femtosecondsPerTick = 0;

void acpi_init_hpet(void){
    hpet = (struct acpi_hpet *)acpi_find_sdt("HPET");
    if (!hpet) {
        serial_printf(LOG_ERROR "HPET Table not found\n");
        return;
    } else {
        serial_printf(LOG_OK "HPET Found\n");
    }

    uint64_t *address = paddr_to_vaddr(hpet->address.address);

    paging_map_page(kernel_page_map, (uint64_t)hpet->address.address, (uint64_t)address, PTE_PRESENT | PTE_WRITABLE);
    
    *(uint32_t*)(address + 0x10)    |= HPET_MAINCOUNTER_ENABLE  |   HPET_LEGACY_REPLACEMENT;
    *(uint32_t*)(address + 0x100)   |= HPET_TIMER_INTERUPTS     |   HPET_TIMER_PERIODIC;

    femtosecondsPerTick = *(uint64_t*)address >> 32;
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
        sched_yield();
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

uint64_t hpet_get_femtoseconds(){
    if (!address || femtosecondsPerTick == 0)
        return 0;
    return *(uint64_t*)(address + 0xF0) * femtosecondsPerTick;
}