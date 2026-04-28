#include <ACPI/acpi.h>
#include <ACPI/acpi_hpet.h>
#include <stdint.h>
#include <stddef.h>
#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <ansii.h>
#include <sched/scheduler.h>
#include <media/bmp/bmp.h>
#include <mm/kalloc.h>
#include <boot/earlyboot_console.h>
#include <string.h>

bmp_file_t* ACPI_get_boot_logo(void) {
    struct acpi_bgrt *bgrt = (struct acpi_bgrt *)acpi_find_sdt("BGRT");
    
    if (!bgrt) {
        serial_printf(LOG_WARN "BGRT table not found, this is not good\n");
        return NULL;
    }

    if (bgrt->type != 0) {
        serial_printf(LOG_ERROR "BGRT image is invalid or not a supported BMP.\n");
        EARLY_FAIL("BGRT image is invalid or not a supported BMP");
        return NULL;
    }

    bmp_file_t *boot_logo = (bmp_file_t *)paddr_to_vaddr(bgrt->image_address);
    
    serial_printf(LOG_OK "Found UEFI boot logo at virtual address: %p\n", boot_logo);
    EARLY_OK("Found a bootloader image!");
    return (bmp_file_t*)paddr_to_vaddr(bgrt->image_address);
}