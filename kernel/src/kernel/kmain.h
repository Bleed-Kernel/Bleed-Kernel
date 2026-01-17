#pragma once
#include <devices/type/tty_device.h>

extern tty_t tty0;

void kernel_self_test();
tty_t kernel_console_init();