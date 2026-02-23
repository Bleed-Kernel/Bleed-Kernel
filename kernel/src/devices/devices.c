#include <devices/devices.h>
#include <status.h>
#include <string.h>
#include <fs/vfs.h>
#include <stdio.h>
#include <mm/spinlock.h>

#define device_from_inode(inode) ((device_t*)inode->internal_data)

static struct {INode_t *inode; char *name;} device_list[MAX_DEVICES];
static size_t device_list_count = 0;   // faster to save hwo many devices we have than check every time we register a new one

static spinlock_t device_list_lock = {0};

/// @brief register a new device
/// @param device device structure
/// @return df
long device_register(INode_t *device, char *name){
    if (!device || !name)
        return -DEV_EXISTS;

    spinlock_acquire(&device_list_lock);
    if (device_list_count >= MAX_DEVICES) {
        spinlock_release(&device_list_lock);
        return -MAX_DEVICES_REACHED;
    }

    size_t devidx = device_list_count;

    for (size_t i = 0; i < device_list_count; i++){
        if (device_list[i].name == name || 
            strcmp(device_list[i].name, name) == 0){
            spinlock_release(&device_list_lock);
            return -DEV_EXISTS;
        }
    }

    INode_t *devdir = NULL;
    path_t devpath = vfs_path_from_abs("/dev");
    int lr = vfs_lookup(&devpath, &devdir);
    if (lr < 0 || !devdir) {
        spinlock_release(&device_list_lock);
        return lr < 0 ? lr : -FILE_NOT_FOUND;
    }

    INode_t *devicenode = NULL;
    char dev_path_buffer[4096] = {0};
    snprintf(dev_path_buffer, sizeof(dev_path_buffer), "/dev/%s", name);
    path_t device_file = vfs_path_from_abs(dev_path_buffer);

    int cr = vfs_create(&device_file, &devicenode, INODE_FILE);
    if (cr < 0 || !devicenode) {
        vfs_drop(devdir);
        spinlock_release(&device_list_lock);
        return cr < 0 ? cr : -OUT_OF_MEMORY;
    }

    devicenode->ops = device->ops;
    devicenode->type = INODE_DEVICE;

    device_list[devidx].inode = device;
    device_list[devidx].name = name;
    device_list_count++;
    vfs_drop(devdir);
    spinlock_release(&device_list_lock);
    return 0;
}

/// @brief get a device by its name
/// @param name in name
/// @return return device structure pointer, null indicates an error.
INode_t *device_get_by_name(const char *name){
    if (!name)
        return NULL;

    for (size_t i = 0; i < device_list_count; i++){
        if (strcmp(device_list[i].name, name) == 0){
            return device_list[i].inode;
        }
    }

    return NULL;
}
