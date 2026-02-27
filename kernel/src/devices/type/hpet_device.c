#include <devices/devices.h>
#include <devices/type/hpet_device.h>
#include <ACPI/acpi_hpet.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <string.h>

static long hpet_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode;
    (void)offset;

    if (!buf || len == 0)
        return 0;

    uint64_t now = hpet_get_femtoseconds();
    if (len > sizeof(now))
        len = sizeof(now);

    umemcpy(buf, &now, len);
    return (long)len;
}

static int hpet_ioctl(INode_t *inode, unsigned long request, void *arg) {
    (void)inode;

    if (request == HPET_IOCTL_GET_FEMTOSECONDS) {
        if (!arg)
            return -1;

        uint64_t now = hpet_get_femtoseconds();
        umemcpy(arg, &now, sizeof(now));
        return 0;
    }

    return -1;
}

static INodeOps_t hpet_inode_ops = {
    .read = hpet_read,
    .ioctl = hpet_ioctl,
};

void hpet_device_init(void) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops = &hpet_inode_ops;
    dev_inode->type = INODE_DEVICE;
    dev_inode->shared = 1;

    device_register(dev_inode, "hpet");
}
