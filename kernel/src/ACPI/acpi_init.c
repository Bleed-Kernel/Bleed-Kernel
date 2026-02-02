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

struct acpi_rsdp *acpi_rsdp = NULL;
struct acpi_fadt *fadt = NULL;

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

static struct acpi_sdt *acpi_find_sdt(const char sig[4]) {
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
}