#include <ACPI/acpi.h>
#include <ACPI/acpi_hpet.h>
#include <stdint.h>

uint64_t sys_femtoseconds(){
    return hpet_get_femtoseconds();
}