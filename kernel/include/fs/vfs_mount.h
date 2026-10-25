#pragma once
#include <stdbool.h>
#include <stddef.h>

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

// mount the filesystem image in the file at file_path, like a loop device.
int vfs_mount_file(const char *path, const char *file_path);

// unmount whatever is at path, -EBUSY while anything inside it is still in use
int vfs_umount(const char *path);

// unmount everything, for when the power is about to go. returns how many wouldnt come off
int vfs_umount_all(void);

// unmount everything sitting on dev_inode even if its busy, for a device thats already gone.
int vfs_umount_device(INode_t *dev_inode);

// true if something mounted from dev_inode has files open or a cwd in it
bool vfs_device_busy(INode_t *dev_inode);

// where dev_inode is mounted, -ENOENT if it isnt
int vfs_device_mount_path(INode_t *dev_inode, char *out, size_t size);

struct user_mount_info;

int vfs_mount_info(int index, struct user_mount_info *out);
