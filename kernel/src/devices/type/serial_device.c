#include <devices/devices.h>
#include <fs/vfs.h>
#include <status.h>
#include <string.h>
#include <mm/kalloc.h>
#include <drivers/serial/serial.h>

static long serial_device_write(INode_t* inode, const void* buf, size_t count, size_t offset) {
    (void)inode; (void)offset;
    if (!buf || count == 0) return 0;

    serial_write_n((const char *)buf, count);
    return count;
}

static const INodeOps_t serial_ops = {
    .write = serial_device_write
};

int serial_device_register() {
    INode_t* dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode) return status_print_error(OUT_OF_MEMORY);

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops = &serial_ops;
    dev_inode->shared = 1;
    dev_inode->type = INODE_DEVICE;

    return device_register(dev_inode, "serial");
}
