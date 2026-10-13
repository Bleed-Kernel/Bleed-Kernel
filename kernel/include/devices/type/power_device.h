#pragma once

#include <stdint.h>

#define POWER_DEVICE_POWEROFF       0xFFFFFFFF
#define POWER_DEVICE_REBOOT         0xEEEEEEEE
#define POWER_DEVICE_KERNEL_PANIC   0xDEADBEEF

#define POWER_TERM_WAIT_MS          3000    // how long tasks get to exit after SIGTERM
#define POWER_KILL_WAIT_MS          1000    // and after the SIGKILL the stragglers get
#define POWER_POLL_MS               50
#define POWER_BUTTON_POLL_MS        250     // how often the power button is looked at

void power_device_init(void);

// entry point of the power watchdog task, a press of the power button becomes a shutdown
void power_button_watch(void);

// ask every task to exit, give them a moment, kill whats left,
// then unmount every filesystem. called by whoever is about to turn the power off or reset,
// that task is left alone
void power_prepare(void);
