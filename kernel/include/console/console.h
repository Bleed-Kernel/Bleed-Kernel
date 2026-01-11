#pragma once
#include <devices/devices.h>
#include <devices/type/tty_device.h>

void console_set(INode_t *console_device, tty_t tty_device);
INode_t *console_get_active_console(void);
tty_t console_get_active_tty(void);

int console_write(const void *string);