#pragma once
#include <stdint.h>
#include <ACPI/acpi_time.h>

void acpi_init(void);
void rtc_get_time(struct rtc_time *t);

__attribute__((noreturn)) void acpi_shutdown(void);
__attribute__((noreturn)) void acpi_reboot(void);