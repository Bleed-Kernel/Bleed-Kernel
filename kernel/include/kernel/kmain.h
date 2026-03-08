#pragma once
#include <devices/type/tty_device.h>

extern tty_t tty0;

void kernel_self_test();
tty_t kernel_console_init();
int kernel_spawn_shell_on_tty(uint32_t tty_index);
void kernel_request_shell_spawn(uint32_t tty_index);
void kernel_service_shell_spawn_requests(void);