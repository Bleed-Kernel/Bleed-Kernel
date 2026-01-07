#include <ACPI/acpi.h>
#include <cpu/io.h>
#include <stddef.h>
#include <panic.h>

#define PM1A_CNT    fadt->pm1a_cnt_blk
#define PM1B_CNT    fadt->pm1b_cnt_blk

#define ACPI_S5    0x5      //Soft Off
// theres some others i dont use for now like s3 and s4

#define SLP_EN     (1 << 13)

#define ACPI_PM1_SLEEP_CMD(slp_typ)  (((slp_typ) << 10) | SLP_EN)

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

struct acpi_fadt {
    struct acpi_sdt header;

    uint32_t firmware_ctrl;
    uint32_t dsdt;

    uint8_t  reserved;

    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;

    uint8_t  S4BIOS_req;
    uint8_t  pstate_cnt;

    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;

    // acpi2
    uint64_t reset_reg_address;
    uint8_t  reset_value;
    uint8_t  reserved2[3];

    // more later acpi is huge

} __attribute__((packed));

struct acpi_gas {
    uint8_t  space_id;    // 0=RAM 1=I/O
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

extern struct acpi_fadt *fadt;