#include <ACPI/acpi.h>
#include <stdio.h>

void sys_reboot(void){
    acpi_reboot();

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}