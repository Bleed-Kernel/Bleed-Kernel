#include <ACPI/acpi.h>
#include <stdio.h>

void sys_reboot(void){
    kprintf("Bleed Kernel, Rebooting Safely!\n");
    acpi_reboot();

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}