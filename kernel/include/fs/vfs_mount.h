#pragma once

#include <fs/vfs.h>
#include <devices/type/blk_device.h>

#define VFS_MAX_MOUNTS 16

typedef enum {
    FS_TYPE_UNKNOWN = -1,
    FS_TYPE_FAT32 = 0,
    FS_TYPE_EXT2  = 1,
    FS_TYPE_EXFAT = 2
} fs_type_t;

// mount block device at path, creates it if it doesnt exist. 0 or a negative errno
int vfs_mount(const char *path, INode_t *dev_inode);

// unmount whatever is at path, -EBUSY while anything inside it is still in use
int vfs_umount(const char *path);
