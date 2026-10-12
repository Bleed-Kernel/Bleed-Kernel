#include <ACPI/acpi.h>
#include <devices/type/power_device.h>
#include <stdio.h>

void sys_reboot(void){
    power_prepare();
    acpi_reboot();

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}