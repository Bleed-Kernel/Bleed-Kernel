#pragma once
#include <ACPI/acpi.h>
#include <stdint.h>

#define femtosecondsPerSecond       1000000000000000
#define femtosecondsPerMillisecond  1000000000000
#define femtosecondsPerMicrosecond  1000000000
#define femtosecondsPerNanosecond   1000

#define HPET_MAINCOUNTER_ENABLE   (1u << 0)
#define HPET_LEGACY_REPLACEMENT   (1u << 1)
#define HPET_TIMER_INTERRUPTS     (1u << 2)
#define HPET_TIMER_PERIODIC       (1u << 3)
#define HPET_TIMER_VAL_SET        (1u << 6)

#define HPET_MAIN_COUNTER       0xF0

#define HPET_ENABLE_CNF   (1 << 0)
#define HPET_LEGACY_CNF   (1 << 1)

extern volatile uint64_t* address;

struct acpi_hpet{
    struct acpi_sdt header;
    uint8_t     hardware_revision_id;
    uint8_t     comparator_count    : 5;
    uint8_t     counter_size        : 1;
    uint8_t     reserved            : 1;
    uint8_t     legacy_replacement  : 1;
    uint16_t    pci_vendor_id;

    struct{
        uint8_t     address_space_id;
        uint8_t     register_bit_width;
        uint8_t     register_bit_offset;
        uint8_t     reserved;
        uint64_t    address;
    }__attribute__((packed)) address;

    uint8_t     hpet_number;
    uint16_t    minimum_tick;
    uint8_t     page_protection;
}__attribute__((packed));

uint64_t hpet_read_counter(void);

uint64_t hpet_get_femtoseconds();

void wait_ns(uint64_t ns);
void wait_us(uint64_t us);
void wait_ms(uint64_t ms);
void wait_s(uint64_t s);

void acpi_init_hpet(void);