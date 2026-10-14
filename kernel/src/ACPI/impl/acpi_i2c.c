#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <ACPI/acpi.h>
#include <ACPI/acpi_i2c.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include "../acpi_priv.h"

// the compressed form of PNP0C50, how an EisaId() ends up in the bytecode
static const uint8_t eisa_hid_over_i2c[4] = { 0x41, 0xD0, 0x0C, 0x50 };

static bool aml_contains(const uint8_t *aml, size_t len, const void *what, size_t what_len) {
    for (size_t i = 0; i + what_len <= len; i++) {
        if (!memcmp(&aml[i], what, what_len))
            return true;
    }
    return false;
}

// 3 letters packed 5 bits each then 4 hex digits
static void aml_eisa_to_string(const uint8_t *id, char out[9]) {
    static const char hex[] = "0123456789ABCDEF";

    out[0] = (char)(0x40 + ((id[0] >> 2) & 0x1F));
    out[1] = (char)(0x40 + (((id[0] & 0x3) << 3) | (id[1] >> 5)));
    out[2] = (char)(0x40 + (id[1] & 0x1F));
    out[3] = hex[id[2] >> 4];
    out[4] = hex[id[2] & 0xF];
    out[5] = hex[id[3] >> 4];
    out[6] = hex[id[3] & 0xF];
    out[7] = '\0';
}

// the _HID of the device a resource at pos sits in. returns where the Name() starts,
// 0 if there isnt one close enough or another device starts in between
static size_t aml_owner_hid(const uint8_t *aml, size_t pos, char out[9]) {
    out[0] = '\0';

    for (size_t back = 5; back <= pos && back <= ACPI_I2C_HID_WINDOW; back++) {
        size_t at = pos - back;
        if (aml[at] != AML_NAME_OP || memcmp(&aml[at + 1], "_HID", 4)) continue;

        static const uint8_t device_op[2] = { AML_EXT_OP, AML_DEVICE_OP };
        if (aml_contains(&aml[at], pos - at, device_op, sizeof(device_op)))
            return 0;

        const uint8_t *val = &aml[at + 5];
        if (*val == AML_DWORD_PREFIX && back >= 10) {
            aml_eisa_to_string(val + 1, out);
        } else if (*val == AML_STRING_PREFIX) {
            for (size_t i = 0; i < 8 && at + 6 + i < pos && val[1 + i]; i++) {
                out[i]     = (char)val[1 + i];
                out[i + 1] = '\0';
            }
        }
        return at;
    }
    return 0;
}

// walk the rest of the _CRS the I2cSerialBus at pos is in for how the device interrupts
static void aml_find_irq(const uint8_t *aml, size_t len, size_t pos, acpi_i2c_dev_t *dev) {
    for (int guard = 0; guard < ACPI_RES_WALK_MAX && pos + 3 <= len; guard++) {
        const uint8_t *res = &aml[pos];

        if (!(res[0] & ACPI_RES_LARGE)) {
            if ((res[0] & ACPI_RES_END_TAG) == ACPI_RES_END_TAG) return;
            pos += 1 + (res[0] & 0x7);
            continue;
        }

        size_t size = 3 + (res[1] | ((size_t)res[2] << 8));
        if (pos + size > len) return;

        if (res[0] == ACPI_RES_EXT_IRQ && size >= ACPI_RES_IRQ_FIRST + 4 && res[ACPI_RES_IRQ_COUNT]) {
            memcpy(&dev->irq, &res[ACPI_RES_IRQ_FIRST], sizeof(dev->irq));
            dev->irq_level = !(res[ACPI_RES_IRQ_FLAGS] & ACPI_RES_IRQ_EDGE);
            dev->irq_low   = res[ACPI_RES_IRQ_FLAGS] & ACPI_RES_IRQ_LOW;
            return;
        }
        if (res[0] == ACPI_RES_GPIO && size >= ACPI_RES_GPIO_PINS + 2 && !res[ACPI_RES_GPIO_TYPE]) {
            size_t pins = res[ACPI_RES_GPIO_PINS] | ((size_t)res[ACPI_RES_GPIO_PINS + 1] << 8);
            if (pins + 2 > size) return;

            dev->gpio_irq = true;
            dev->gpio_pin = res[pins] | (res[pins + 1] << 8);
            return;
        }
        pos += size;
    }
}

static void acpi_i2c_scan(const uint8_t *aml, size_t len, acpi_i2c_dev_t *out,
                          size_t max, size_t *count) {
    for (size_t pos = 0; pos + ACPI_RES_I2C_MIN_LENGTH <= len; pos++) {
        const uint8_t *res = &aml[pos];

        // its just bytes in a buffer, so only believe the ones where every field makes sense
        if (res[0] != ACPI_RES_SERIAL_BUS || res[ACPI_RES_I2C_BUS_TYPE] != ACPI_SERIAL_BUS_I2C) continue;
        if (res[ACPI_RES_I2C_REVISION] < 1 || res[ACPI_RES_I2C_REVISION] > 2) continue;
        if (res[ACPI_RES_I2C_TYPE_REV] != 1) continue;

        uint16_t type_len = res[ACPI_RES_I2C_TYPE_LEN] | (res[ACPI_RES_I2C_TYPE_LEN + 1] << 8);
        uint32_t speed;
        memcpy(&speed, &res[ACPI_RES_I2C_SPEED], sizeof(speed));
        uint16_t addr = res[ACPI_RES_I2C_ADDRESS] | (res[ACPI_RES_I2C_ADDRESS + 1] << 8);

        if (type_len < 6 || speed < 10000 || speed > 3400000) continue;
        if (addr < 0x08 || addr > 0x77) continue;

        acpi_i2c_dev_t dev = { .addr = addr };
        size_t hid_at = aml_owner_hid(aml, pos, dev.hid);
        if (hid_at)
            dev.hid_over_i2c = aml_contains(&aml[hid_at], pos - hid_at, eisa_hid_over_i2c, 4) ||
                               aml_contains(&aml[hid_at], pos - hid_at, ACPI_HID_OVER_I2C, 7);
        aml_find_irq(aml, len, pos, &dev);

        bool seen = false;
        for (size_t i = 0; i < *count; i++) {
            if (out[i].addr == dev.addr && !strcmp(out[i].hid, dev.hid))
                seen = true;
        }
        if (!seen && *count < max)
            out[(*count)++] = dev;
    }
}

static void acpi_i2c_scan_table(paddr_t phys, acpi_i2c_dev_t *out, size_t max, size_t *count) {
    if (!phys || paging_map_mmio(phys, sizeof(struct acpi_sdt)) < 0) return;

    struct acpi_sdt *tbl = paddr_to_vaddr(phys);
    if (memcmp(tbl->signature, "DSDT", 4) && memcmp(tbl->signature, "SSDT", 4)) return;
    if (tbl->length <= sizeof(*tbl) || paging_map_mmio(phys, tbl->length) < 0) return;

    acpi_i2c_scan((const uint8_t *)tbl + sizeof(*tbl), tbl->length - sizeof(*tbl), out, max, count);
}

size_t acpi_i2c_devices(acpi_i2c_dev_t *out, size_t max) {
    size_t count = 0;
    if (!fadt || !out) return 0;

    // the DSDT isnt in the table list, the FADT points at it
    paddr_t dsdt = fadt->dsdt;
    if (!dsdt && fadt->header.length >= offsetof(struct acpi_fadt, x_dsdt) + sizeof(fadt->x_dsdt))
        dsdt = (paddr_t)fadt->x_dsdt;
    acpi_i2c_scan_table(dsdt, out, max, &count);

    struct acpi_sdt *tbl;
    for (size_t i = 0; (tbl = acpi_get_sdt(i)); i++)
        acpi_i2c_scan_table(vaddr_to_paddr(tbl), out, max, &count);

    return count;
}
