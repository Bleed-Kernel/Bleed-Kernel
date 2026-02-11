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
    if (!device)
        return -DEV_EXISTS;
    if (device_list_count >= MAX_DEVICES)
        return -MAX_DEVICES_REACHED;

    spinlock_acquire(&device_list_lock);
    int devidx = device_list_count;

    for (size_t i = 0; i < device_list_count; i++){
        if (device_list[i].name == name || 
            strcmp(device_list[i].name, name) == 0){
            return -DEV_EXISTS;
        }
    }

    INode_t *devdir = NULL;
    path_t devpath = vfs_path_from_abs("/dev");
    vfs_lookup(&devpath, &devdir);

    INode_t *devicenode = device;
    char dev_path_buffer[4096] = {0};
    snprintf(dev_path_buffer, sizeof(dev_path_buffer), "/dev/%s", name);
    path_t device_file = vfs_path_from_abs(dev_path_buffer);

    vfs_create(&device_file, &devicenode, INODE_DEVICE);

    device_list[devidx].inode = device;
    device_list[devidx].name = name;
    device_list_count++;
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