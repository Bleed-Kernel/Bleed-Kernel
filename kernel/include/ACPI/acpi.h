#pragma once
#include <stdint.h>

void acpi_init(void);

__attribute__((noreturn)) void acpi_shutdown(void);
__attribute__((noreturn)) void acpi_reboot(void);