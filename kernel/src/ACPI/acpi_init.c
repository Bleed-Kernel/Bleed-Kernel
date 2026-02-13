#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <cpu/io.h>
#include <panic.h>
#include <ACPI/acpi.h>
#include <stdio.h>
#include <mm/pmm.h>
#include <ansii.h>
#include <panic.h>
#include <drivers/serial/serial.h>
#include "acpi_priv.h"

#define MAX_ISO 16

struct acpi_rsdp *acpi_rsdp = NULL;
struct acpi_fadt *fadt = NULL;

static struct acpi_madt *madt = NULL;

static uint64_t lapic_base = 0;
static uint64_t ioapic_base = 0;
static uint32_t ioapic_gsi_base = 0;

static struct {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} iso_table[MAX_ISO];

static size_t iso_count = 0;

static int acpi_checksum(void *ptr, size_t len) {
    uint8_t sum = 0;
    uint8_t *p = ptr;
    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return sum == 0;
}

static void *acpi_get_rsdp(void) {
    extern volatile struct limine_rsdp_request rsdp_request;

    if (!rsdp_request.response) {
            ke_panic("ACPI: No RSDP response from bootloader");
        }
    if (!rsdp_request.response->address) {
            ke_panic("ACPI: Invalid RSDP address from bootloader");
        }

    return rsdp_request.response->address;
}

struct acpi_sdt *acpi_find_sdt(const char sig[4]) {
    if (acpi_rsdp->revision >= 2 && acpi_rsdp->xsdt_address) {
        struct acpi_xsdt *xsdt = paddr_to_vaddr(acpi_rsdp->xsdt_address);
        if (!acpi_checksum(xsdt, xsdt->header.length))
            return NULL;

        if (xsdt->header.length < sizeof(struct acpi_sdt)) {
            serial_printf(LOG_ERROR "ACPI: Invalid XSDT length\n");
            return NULL;
        }

        size_t entries = (xsdt->header.length - sizeof(struct acpi_sdt)) / 8;
        for (size_t i = 0; i < entries; i++) {
            struct acpi_sdt *tbl = paddr_to_vaddr(xsdt->tables[i]);
            if (!tbl) {
                ke_panic("ACPI: Invalid table virtual address");
            }
            if (!memcmp(tbl->signature, sig, 4))
                return tbl;
        }
    }

    struct acpi_sdt *rsdt = paddr_to_vaddr(acpi_rsdp->rsdt_address);
    if (!acpi_checksum(rsdt, rsdt->length))
        return NULL;

    uint32_t *tables = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt));
    size_t entries = (rsdt->length - sizeof(struct acpi_sdt)) / 4;
    for (size_t i = 0; i < entries; i++) {
        struct acpi_sdt *tbl = paddr_to_vaddr(tables[i]);
        if (!memcmp(tbl->signature, sig, 4))
            return tbl;
    }

    return NULL;
}

void acpi_init(void) {
    acpi_rsdp = acpi_get_rsdp();

    if (!acpi_rsdp)
        ke_panic("ACPI: no RSDP");

    if (acpi_rsdp->revision == 0) {
        kprintf(LOG_INFO "ACPI Version: 1.0\n");
    } else {
        kprintf(LOG_INFO "ACPI Version: 2.0 or higher (revision %u)\n", acpi_rsdp->revision);
    }

    size_t rsdp_len = acpi_rsdp->revision >= 2 ? acpi_rsdp->length : 20;
    if (!acpi_checksum(acpi_rsdp, rsdp_len))
        ke_panic("ACPI: bad RSDP checksum");

    fadt = (struct acpi_fadt *)acpi_find_sdt("FACP");
    if (!fadt)
        ke_panic("ACPI: FADT not found");

    if (!acpi_checksum(fadt, fadt->header.length))
        ke_panic("ACPI: bad FADT checksum");

    if (fadt->smi_cmd && fadt->acpi_enable) {
        serial_printf(LOG_INFO "ACPI: Enabling via SMI_CMD=0x%x\n", fadt->smi_cmd);
        outb(fadt->smi_cmd, fadt->acpi_enable);

        int timeout = 1000000;
        while (!(inw(fadt->pm1a_cnt_blk) & 1) && timeout-- > 0)
            asm("pause");
        if (timeout <= 0) {
            ke_panic("ACPI: Enable timeout");
        }
    }

    madt = (struct acpi_madt *)acpi_find_sdt("APIC");
    if (!madt)
        ke_panic("ACPI: MADT not found");

    if (!acpi_checksum(madt, madt->header.length))
        ke_panic("ACPI: bad MADT checksum");

    lapic_base = madt->lapic_addr;

    uint8_t *ptr = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        struct madt_entry_header *h = (void *)ptr;
        if (h->length < sizeof(struct madt_entry_header))
            break;
        if (ptr + h->length > end)
            break;

        switch (h->type) {

            case 1: {
                struct madt_ioapic *io = (void *)ptr;
                ioapic_base = io->ioapic_addr;
                ioapic_gsi_base = io->gsi_base;
                break;
            }

            case 2: {
                if (iso_count < MAX_ISO) {
                    struct madt_iso *iso = (void *)ptr;
                    iso_table[iso_count].source = iso->source;
                    iso_table[iso_count].gsi    = iso->gsi;
                    iso_table[iso_count].flags  = iso->flags;
                    iso_count++;
                }
                break;
            }

            case 5: {
                struct madt_lapic_addr_override *ov = (void *)ptr;
                lapic_base = ov->lapic_addr;
                break;
            }

            default:
                break;
        }

        ptr += h->length;
    }

    if (!lapic_base)
        ke_panic("ACPI: No LAPIC base");

    if (!ioapic_base)
        ke_panic("ACPI: No IOAPIC base");

    serial_printf(LOG_INFO "ACPI: LAPIC @ 0x%p\n", lapic_base);
    serial_printf(LOG_INFO "ACPI: IOAPIC @ 0x%p (GSI base %u)\n", ioapic_base, ioapic_gsi_base);

    apic_init();
}

uint64_t acpi_lapic_base(void) {
    return lapic_base;
}

uint64_t acpi_ioapic_base(void) {
    return ioapic_base;
}

uint32_t acpi_ioapic_gsi_base(void) {
    return ioapic_gsi_base;
}

uint32_t acpi_irq_to_gsi(uint32_t irq) {
    for (size_t i = 0; i < iso_count; i++) {
        if (iso_table[i].source == irq)
            return iso_table[i].gsi;
    }
    return irq;
}

uint16_t acpi_irq_flags(uint32_t irq) {
    for (size_t i = 0; i < iso_count; i++) {
        if (iso_table[i].source == irq)
            return iso_table[i].flags;
    }
    return 0;
}
