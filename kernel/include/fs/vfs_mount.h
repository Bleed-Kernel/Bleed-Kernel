#pragma once

#include <fs/vfs.h>
#include <devices/type/blk_device.h>

#define VFS_MAX_MOUNTS 16

typedef enum {
    FS_TYPE_UNKNOWN = -1,
    FS_TYPE_FAT32 = 0,
    FS_TYPE_EXT2  = 1,
} fs_type_t;

// mount block device at path, creates it if it doesnt exist
int vfs_mount(const char *path, INode_t *dev_inode);

// unmount whatever is at path, will drop it too
int vfs_umount(const char *path);

//returns the mount root if `inode` is a mount point, otherwise NULL.
INode_t *vfs_mount_resolve(INode_t *inode);

//Retrieve the blk_device_t from a /dev/hdXN inode (if it is one) or will return NULL
blk_device_t *vfs_inode_to_blk(INode_t *inode);