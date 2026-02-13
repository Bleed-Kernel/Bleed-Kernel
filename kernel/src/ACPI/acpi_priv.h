#include <ACPI/acpi.h>
#include <cpu/io.h>
#include <stddef.h>
#include <panic.h>

#define PM1A_CNT    fadt->pm1a_cnt_blk
#define PM1B_CNT    fadt->pm1b_cnt_blk

#define ACPI_S5    0x5
#define SLP_EN     (1 << 13)

#define ACPI_PM1_SLEEP_CMD(slp_typ)  (((slp_typ) << 10) | SLP_EN)

typedef struct
{
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) GenericAddressStructure;

typedef struct acpi_fadt
{
    struct acpi_sdt header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t __reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t pm1_evt_len;
    uint8_t pm1_cnt_len;
    uint8_t pm2_cnt_len;
    uint8_t pm_tmr_len;
    uint8_t gpe0_blk_len;
    uint8_t gpe1_blk_len;
    uint8_t gpe1_base;
    uint8_t cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alrm;
    uint8_t mon_alrm;
    uint8_t century;
    uint16_t iapc_boot_arch;
    uint8_t __reserved2;
    uint32_t flags;
    GenericAddressStructure reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    uint8_t x_pm1a_evt_blk[12];
    uint8_t x_pm1b_evt_blk[12];
    uint8_t x_pm1a_cnt_blk[12];
    uint8_t x_pm1b_cnt_blk[12];
    uint8_t x_pm2_cnt_blk[12];
    uint8_t x_pm_tmr_blk[12];
    uint8_t x_gpe0_blk[12];
    uint8_t x_gpe1_blk[12];
    uint8_t sleep_control_reg[12];
    uint8_t sleep_status_reg[12];
    uint64_t hypervison_vendor_identity;
} __attribute__((packed)) fadt_t;

struct acpi_madt {
    struct acpi_sdt header;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic {
    uint8_t  type;
    uint8_t  length;
    uint8_t  acpi_cpu_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
    uint8_t  type;
    uint8_t  length;
    uint8_t  ioapic_id;
    uint8_t  reserved;
    uint32_t ioapic_addr;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    uint8_t  type;
    uint8_t  length;
    uint8_t  bus;
    uint8_t  source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct madt_lapic_addr_override {
    uint8_t  type;
    uint8_t  length;
    uint16_t reserved;
    uint64_t lapic_addr;
} __attribute__((packed));

uint64_t acpi_lapic_base(void);
uint64_t acpi_ioapic_base(void);
uint32_t acpi_ioapic_gsi_base(void);
uint32_t acpi_irq_to_gsi(uint32_t irq);
uint16_t acpi_irq_flags(uint32_t irq);

extern struct acpi_rsdp *acpi_rsdp;
extern struct acpi_fadt *fadt;