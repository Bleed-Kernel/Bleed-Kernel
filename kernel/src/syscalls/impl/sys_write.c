#include <string.h>
#include <devices/devices.h>
#include <mm/kalloc.h>
#include <devices/type/tty_device.h>

uint64_t sys_write(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (!user_buf || len == 0)
        return 0;

    INode_t *dev = NULL;

    switch (fd) {
    case 1: case 2:
        dev = device_get_by_name("tty0");
        break;
    default:
        return (uint64_t)-1;
    }

    if (!dev || !dev->ops->write)
        return (uint64_t)-1;

    char *kbuf = (char *)kmalloc(len);
    if (!kbuf) return -1;

    memcpy(kbuf, (const void *)user_buf, len);

    uint64_t written = dev->ops->write(dev, kbuf, len, 0);

    kfree(kbuf, len);
    return written;
}
