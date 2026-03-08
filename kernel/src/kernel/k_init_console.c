#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <console/console.h>

tty_t kernel_console_init(){
    (void)tty_create_framebuffer(NULL);

    (void)tty_set_active_index(0);
    INode_t *tty0_node = device_get_by_name("tty0");
    if (!tty0_node || !tty0_node->internal_data) {
        tty_t empty = {0};
        return empty;
    }
    return *(tty_t *)tty0_node->internal_data;
}
