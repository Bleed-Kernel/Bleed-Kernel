#include <ACPI/acpi.h>
#include <cpu/io.h>
#include <stdio.h>

void sys_shutdown(void) {
    acpi_shutdown();

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}