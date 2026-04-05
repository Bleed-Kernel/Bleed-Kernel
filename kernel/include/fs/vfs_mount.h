#pragma once

#include <fs/vfs.h>
#include <devices/type/blk_device.h>

#define VFS_MAX_MOUNTS 16

// mount block device at path, creates it if it doesnt exist
int vfs_mount(const char *path, blk_device_t *blk);

// unmount whatever is at path, will drop it too
int vfs_umount(const char *path);

//returns the mount root if `inode` is a mount point, otherwise NULL.
INode_t *vfs_mount_resolve(INode_t *inode);

//Retrieve the blk_device_t from a /dev/hdXN inode (if it is one) or will return NULL
blk_device_t *vfs_inode_to_blk(INode_t *inode);