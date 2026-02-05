#pragma once
#include <stdint.h>
#include <ACPI/acpi_time.h>

struct acpi_sdt {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;

    // acpi2
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_xsdt {
    struct acpi_sdt header;
    uint64_t tables[];
} __attribute__((packed));

void acpi_init(void);
void rtc_get_time(struct rtc_time *t);

struct acpi_sdt *acpi_find_sdt(const char sig[4]);

__attribute__((noreturn)) void acpi_shutdown(void);
__attribute__((noreturn)) void acpi_reboot(void);