#include <devices/devices.h>
#include <fs/vfs.h>
#include <user/errno.h>
#include <string.h>
#include <mm/kalloc.h>
#include <drivers/serial/serial.h>
#include <stdio.h>

static long serial_device_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    (void)inode; (void)offset;
    if (!buf || count == 0) return 0;

    serial_write_n((const char *)buf, count);
    return count;
}

static long serial_device_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode;
    if (!buf || len == 0) return 0;
    return (long)serial_log_read((char *)buf, len, offset);
}

static size_t serial_device_size(INode_t *inode) {
    (void)inode;
    return serial_log_size();
}

static const INodeOps_t serial_ops = {
    .write = serial_device_write,
    .read = serial_device_read,
    .size = serial_device_size
};

int serial_device_register() {
    INode_t* dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode) return -ENOMEM;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops = &serial_ops;
    dev_inode->shared = 1;
    dev_inode->type = INODE_DEVICE;

    return device_register(dev_inode, "serial");
}
