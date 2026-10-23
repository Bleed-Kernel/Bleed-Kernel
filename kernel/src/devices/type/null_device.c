#include <devices/devices.h>
#include <devices/type/null_device.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <string.h>

static long null_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode; (void)buf; (void)len; (void)offset;
    return 0;
}

static long null_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    (void)inode; (void)buf; (void)offset;
    return (long)len;
}

static long zero_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode; (void)offset;
    if (!buf)
        return 0;
    memset(buf, 0, len);
    return (long)len;
}

static INodeOps_t null_inode_ops = {
    .read = null_read,
    .write = null_write,
};

static INodeOps_t zero_inode_ops = {
    .read = zero_read,
    .write = null_write,
};

static void register_one(INodeOps_t *ops, char *name) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops = ops;
    dev_inode->type = INODE_DEVICE;
    dev_inode->shared = 1;

    device_register(dev_inode, name);
}

void null_device_init(void) {
    register_one(&null_inode_ops, "null");
    register_one(&zero_inode_ops, "zero");
}
