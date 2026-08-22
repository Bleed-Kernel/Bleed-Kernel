#include <devices/devices.h>
#include <ACPI/acpi.h>
#include <fs/vfs.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <devices/type/power_device.h>
#include <drivers/serial/serial.h>
#include <string.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <kernel/exception/panic.h>

static int power_ioctl(INode_t *inode, unsigned long request, void *arg) {
    (void)inode;
    (void)arg;

    if (request == POWER_DEVICE_POWEROFF) {
        serial_printf(LOG_OK "Power Device recieved Poweroff instruction\n");
        acpi_shutdown();
        serial_printf(LOG_ERROR "acpi_shutdown() returned unexpectedly\n");
        return -EIO;
    }else if (request == POWER_DEVICE_REBOOT){
        serial_printf(LOG_OK "Power Device recieved Reboot instruction\n");
        acpi_reboot();
        serial_printf(LOG_ERROR "acpi_reboot() returned unexpectedly\n");
        return -EIO;
    }else if (request == POWER_DEVICE_KERNEL_PANIC){
        serial_printf(LOG_OK "Power Device recieved Kernel Panic instruction");
        ke_panic(NULL, "Manually Initiated Crash");
        serial_printf(LOG_ERROR "ke_panic() returned? how did it manage that?\n");
        return -EIO;
    }

    return -EINVAL;
}

static INodeOps_t power_inode_ops = {
    .ioctl = power_ioctl,
};

void power_device_init(void) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops = &power_inode_ops;
    dev_inode->type = INODE_DEVICE;
    dev_inode->shared = 1;

    device_register(dev_inode, "power");
}