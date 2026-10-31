#include <ACPI/acpi.h>
#include <ACPI/acpi_ventoy.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ansii.h>
#include <drivers/serial/serial.h>

static char image_path[VENTOY_PATH_MAX];

void acpi_init_ventoy(void) {
    struct acpi_sdt *tbl = acpi_find_sdt("VTOY");
    if (!tbl)
        return;     // not booted by ventoy, or it was told not to add the table

    if (tbl->length < sizeof(*tbl) + sizeof(ventoy_os_param_t)) {
        serial_printf(LOG_WARN "ventoy: VTOY table too short (%u bytes)\n", tbl->length);
        return;
    }

    // its right after the header, but dont count on it
    const uint8_t           *body  = (const uint8_t *)(tbl + 1);
    size_t                   len   = tbl->length - sizeof(*tbl);
    const ventoy_os_param_t *param = NULL;
    for (size_t off = 0; off + sizeof(ventoy_os_param_t) <= len; off++) {
        if (!memcmp(body + off, VENTOY_GUID, 16)) {
            param = (const ventoy_os_param_t *)(body + off);
            break;
        }
    }
    if (!param) {
        serial_printf(LOG_WARN "ventoy: VTOY table without the ventoy guid in it\n");
        return;
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(*param); i++)
        sum += ((const uint8_t *)param)[i];
    if (sum != 0) {
        serial_printf(LOG_WARN "ventoy: os param checksum is off (0x%x)\n", sum);
        return;
    }

    memcpy(image_path, param->img_path, sizeof(image_path) - 1);
    image_path[sizeof(image_path) - 1] = '\0';
    serial_printf(LOG_OK "ventoy: booted %s from partition %u (%s, %u MiB image)\n",
                  image_path, param->part_id, param->part_type == 0 ? "exfat" : "ntfs",
                  (uint32_t)(param->img_size >> 20));
}

const char *ventoy_image_path(void) {
    return image_path[0] ? image_path : NULL;
}
