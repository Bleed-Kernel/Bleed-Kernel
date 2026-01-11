#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <string.h>
#include <stddef.h>

static tty_t active_tty;
static INode_t *active_console = NULL;

void console_set(INode_t *console_device, tty_t tty_device){
    active_console = console_device;
    active_tty = tty_device;
    return;
}

INode_t *console_get_active_console(void){
    return active_console;
}
tty_t console_get_active_tty(void){
    return active_tty;
}

int console_write(const void *string){
    if (!active_console || !active_console->ops->write) return -1;
    return active_console->ops->write(active_console, string, strlen(string), 0);
}