#include <random/random.h>
#include <devices/devices.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <string.h>

static long random_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode; (void)offset;
    if (!buf)
        return 0;
    random_get_bytes(buf, len);
    return (long)len;
}

// written data gets stirred in
static long random_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    (void)inode; (void)offset;
    if (buf && len)
        random_add(buf, len);
    return (long)len;
}

static INodeOps_t random_inode_ops = {
    .read  = random_read,
    .write = random_write,
};

static void register_one(char *name) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops    = &random_inode_ops;
    dev_inode->type   = INODE_DEVICE;
    dev_inode->shared = 1;
    device_register(dev_inode, name);
}

void random_device_init(void) {
    register_one("random");
    register_one("urandom");
}
