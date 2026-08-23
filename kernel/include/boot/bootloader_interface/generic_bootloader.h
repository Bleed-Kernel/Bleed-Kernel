#pragma once

#include <stdint.h>
#include <stddef.h>

typedef enum bootloader_type {
    BOOTLOADER_UNKNOWN = 0,
    BOOTLOADER_LIMINE,
    BOOTLOADER_GRUB2,
} bootloader_type_t;

typedef enum gbi_memmap_type {
    GBI_MEMMAP_USABLE = 0,
    GBI_MEMMAP_RESERVED,
    GBI_MEMMAP_ACPI_RECLAIMABLE,
    GBI_MEMMAP_ACPI_NVS,
    GBI_MEMMAP_BAD_MEMORY,
    GBI_MEMMAP_BOOTLOADER_RECLAIMABLE,
    GBI_MEMMAP_KERNEL_AND_MODULES,
    GBI_MEMMAP_FRAMEBUFFER,
} gbi_memmap_type_t;

typedef struct gbi_memmap_entry {
    uint64_t base;
    uint64_t length;
    gbi_memmap_type_t type;
} gbi_memmap_entry_t;

typedef struct gbi_framebuffer {
    uint64_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t bpp;
    int      present;
} gbi_framebuffer_t;

typedef struct gbi_module {
    uint64_t    address;
    uint64_t    size;
    const char *path;
    const char *cmdline;
} gbi_module_t;

#define GBI_MAX_MEMMAP_ENTRIES 256
#define GBI_MAX_MODULES        32

typedef struct generic_bootloader_information {
    bootloader_type_t bootloader;
    const char *bootloader_name;
    const char *bootloader_version;

    uint64_t hhdm_offset;

    gbi_framebuffer_t framebuffer;

    gbi_memmap_entry_t memmap[GBI_MAX_MEMMAP_ENTRIES];
    size_t             memmap_entry_count;

    gbi_module_t modules[GBI_MAX_MODULES];
    size_t       module_count;

    uint64_t rsdp_address;

    const char *cmdline;

    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;
} gbi_t;

extern gbi_t g_gbi;

void gbi_init(void);

const char *gbi_bootloader_name(bootloader_type_t type);