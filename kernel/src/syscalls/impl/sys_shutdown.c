#include <ACPI/acpi.h>
#include <devices/type/power_device.h>
#include <cpu/io.h>
#include <stdio.h>

void sys_shutdown(void) {
    power_prepare();
    acpi_shutdown();

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}