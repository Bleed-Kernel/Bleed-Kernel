#pragma once
#include <ACPI/acpi.h>
#include <stdint.h>
#include <stdbool.h>

/*
    Ventoy is very helpful here
    it leave the ISO in an ACPI table called VTOY which we can
    use here
*/
#define VENTOY_GUID         "  www.ventoy.net"
#define VENTOY_PATH_MAX     384

typedef struct __attribute__((packed)) {
    uint8_t  guid[16];
    uint8_t  chksum;                    // all 512 bytes add up to 0
    uint8_t  disk_guid[16];
    uint64_t disk_size;
    uint16_t part_id;
    uint16_t part_type;                 // 0 exfat, 1 ntfs
    char     img_path[VENTOY_PATH_MAX];
    uint64_t img_size;
    uint64_t img_location_addr;
    uint32_t img_location_len;
    uint64_t reserved0[4];
    uint8_t  disk_signature[4];
    uint8_t  reserved1[27];
} ventoy_os_param_t;

_Static_assert(sizeof(ventoy_os_param_t) == 512, "ventoy os param is 512 bytes");

// read the VTOY table if ventoy booted us, call after acpi_init
void acpi_init_ventoy(void);
const char *ventoy_image_path(void);
