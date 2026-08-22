#pragma once

#include <stdint.h>

#define POWER_DEVICE_POWEROFF       0xFFFFFFFF
#define POWER_DEVICE_REBOOT         0xEEEEEEEE
#define POWER_DEVICE_KERNEL_PANIC   0xDEADBEEF

void power_device_init(void);
