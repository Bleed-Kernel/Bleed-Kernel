#include <string.h>
#include <devices/devices.h>
#include <mm/kalloc.h>
#include <devices/type/tty_device.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len){
    if (!user_buf || len == 0)
        return 0;

    device_t *dev = NULL;

    switch (fd) {
    case 0:
        dev = device_get_by_name("tty0");
        break;
    default:
        return (uint64_t)-1;
    }

    if (!dev || !dev->read)
        return (uint64_t)-1;

    return dev->read(dev, (void *)user_buf, len);
}