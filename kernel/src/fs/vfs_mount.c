#include <fs/vfs_mount.h>
#include <fs/vfs.h>
#include <fs/fat32/fat32.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>

typedef struct {
    INode_t  *mount_point;   // tempfs dir inode being covered
    INode_t  *fs_root;       // root of the fs
    INode_t  *dev_inode;     // the /dev/hdaX or /dev/sdaX inode
    bool      active;
} mount_entry_t;

static mount_entry_t mount_table[VFS_MAX_MOUNTS];
static spinlock_t    mount_lock = {0};

INode_t *vfs_mount_resolve(INode_t *inode) {
    if (!inode) return NULL;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].mount_point == inode)
            return mount_table[i].fs_root;
    }
    return NULL;
}

int vfs_mount(const char *path, INode_t *dev_inode) {
    if (!path || !dev_inode) return -1;

    spinlock_acquire(&mount_lock);
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_release(&mount_lock);
        serial_printf(LOG_ERROR "vfs_mount: mount table full\n");
        return -1;
    }
    spinlock_release(&mount_lock);

    // Create mount point dir if it doesn't exist
    path_t p = vfs_path_from_abs(path);
    INode_t *mp = NULL;
    if (vfs_lookup(&p, &mp) < 0) {
        INode_t *created = NULL;
        if (vfs_create(&p, &created, INODE_DIRECTORY) < 0) {
            serial_printf(LOG_ERROR "vfs_mount: could not create mount point %s\n", path);
            return -1;
        }
        vfs_drop(created);
        if (vfs_lookup(&p, &mp) < 0) {
            serial_printf(LOG_ERROR "vfs_mount: could not look up mount point %s\n", path);
            return -1;
        }
    }

    // mount the fat32
    INode_t *fs_root = NULL;
    if (fat32_mount(dev_inode, &fs_root) < 0) {
        vfs_drop(mp);
        serial_printf(LOG_ERROR "vfs_mount: fat32_mount failed for %s\n", path);
        return -1;
    }

    // Point the FAT32 parent at the mount points parent
    fs_root->parent = mp->parent;

    spinlock_acquire(&mount_lock);
    mount_table[slot].mount_point = mp;
    mount_table[slot].fs_root     = fs_root;
    mount_table[slot].dev_inode   = dev_inode;
    mount_table[slot].active      = true;
    spinlock_release(&mount_lock);

    vfs_drop(mp);   //Caller is done here

    serial_printf(LOG_OK "vfs_mount: mounted FAT32 at %s\n", path);
    return 0;
}

int vfs_umount(const char *path) {
    if (!path) return -1;

    path_t p = vfs_path_from_abs(path);
    INode_t *mp = NULL;
    if (vfs_lookup(&p, &mp) < 0) return -1;

    spinlock_acquire(&mount_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active && mount_table[i].mount_point == mp) {

             // drop the fs root
            vfs_drop(mount_table[i].fs_root);
            mount_table[i].active = false;
            spinlock_release(&mount_lock);
            vfs_drop(mp);
            serial_printf(LOG_OK "vfs_umount: unmounted %s\n", path);
            return 0;
        }
    }
    spinlock_release(&mount_lock);
    vfs_drop(mp);
    return -1;
}