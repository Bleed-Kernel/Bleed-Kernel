#include <fs/vfs_mount.h>
#include <fs/vfs.h>
#include <fs/fat32/fat32.h>
#include <fs/ext2/ext2.h>
#include <fs/exfat/exfat.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <user/errno.h>

#define PROBE_FAT32_ROOT_ENTRY_OFF  17    //root entry count
#define PROBE_FAT32_SIG_OFF         510   // 0x55 0xAA boot signature
#define PROBE_EXT2_MAGIC_OFF        (1024 + 56)
#define PROBE_EXT2_MAGIC            0xEF53
#define PROBE_EXFAT_NAME_OFF        3     // "EXFAT   " right after jump_boot[3]
#define PROBE_EXFAT_NAME            "EXFAT   "

/*
 * Same idea as linux: a mount is the pair (directory it covers, root of the filesystem on top).
 * The covered directory carries a pointer to that root, so the path walk only has to look at
 * the inode it just stepped onto instead of comparing names against a table. The mount holds
 * a ref on the directory for as long as it lasts, that pins it in the dentry cache so every
 * lookup lands on the same inode. Going up out of a mount works because the root is given the
 * covered directory's parent as its own.
 */
typedef struct {
    char      mount_path[PATH_MAX];
    INode_t  *mountpoint;   // the directory we cover, pinned by a ref
    INode_t  *fs_root;      // root of the mounted filesystem, the mount owns one ref
    INode_t  *dev_inode;
    fs_type_t fs_type;
    bool      active;
} mount_entry_t;

static mount_entry_t mount_table[VFS_MAX_MOUNTS];
static spinlock_t    mount_lock = {0};

static fs_type_t vfs_detect_fs(INode_t *dev_inode) {
    //ext2 probe using superblock
    uint16_t ext2_magic = 0;
    if (inode_read(dev_inode, &ext2_magic, sizeof(ext2_magic),
                   PROBE_EXT2_MAGIC_OFF) == (long)sizeof(ext2_magic)) {
        if (ext2_magic == PROBE_EXT2_MAGIC) {
            serial_printf(LOG_INFO "vfs_mount: detected ext2\n");
            return FS_TYPE_EXT2;
        }
    }

    // exFAT probe "EXFAT   " right after jump_boot[3]
    uint8_t sector0[512];
    if (inode_read(dev_inode, sector0, sizeof(sector0), 0) == (long)sizeof(sector0)) {
        if (memcmp(sector0 + PROBE_EXFAT_NAME_OFF, PROBE_EXFAT_NAME, 8) == 0) {
            serial_printf(LOG_INFO "vfs_mount: detected exFAT\n");
            return FS_TYPE_EXFAT;
        }

        // FAT32 Probe sector 0 must end in 0x55 0xAA
        uint16_t root_entry_count;
        memcpy(&root_entry_count, sector0 + PROBE_FAT32_ROOT_ENTRY_OFF, 2);
        if (sector0[PROBE_FAT32_SIG_OFF]     == 0x55 &&
            sector0[PROBE_FAT32_SIG_OFF + 1] == 0xAA &&
            root_entry_count == 0) {
            serial_printf(LOG_INFO "vfs_mount: detected FAT32\n");
            return FS_TYPE_FAT32;
        }
    }

    return FS_TYPE_UNKNOWN;
}

// give a slot back after a mount that didnt make it
static void mount_slot_release(int slot) {
    spinlock_acquire(&mount_lock);
    mount_table[slot].active = false;
    spinlock_release(&mount_lock);
}

int vfs_mount(const char *path, INode_t *dev_inode) {
    if (!path || !dev_inode) return -EINVAL;

    spinlock_acquire(&mount_lock);
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_release(&mount_lock);
        serial_printf(LOG_ERROR "vfs_mount: mount table full\n");
        return -ENOMEM;
    }
    mount_table[slot].active     = true;
    mount_table[slot].fs_root    = NULL;
    mount_table[slot].mountpoint = NULL;
    spinlock_release(&mount_lock);

    fs_type_t fs_type = vfs_detect_fs(dev_inode);
    if (fs_type == FS_TYPE_UNKNOWN) {
        mount_slot_release(slot);
        serial_printf(LOG_ERROR "vfs_mount: Unsupported filesystem\n");
        return -EINVAL;
    }

    path_t p = vfs_path_from_abs(path);
    INode_t *mp = NULL;
    if (vfs_lookup(&p, &mp) < 0) {
        if (vfs_create(&p, &mp, INODE_DIRECTORY) < 0) {
            mount_slot_release(slot);
            serial_printf(LOG_ERROR "vfs_mount: could not create mount point %s\n", path);
            return -ENOENT;
        }
    }

    if (mp->type != INODE_DIRECTORY) {
        vfs_drop(mp);
        mount_slot_release(slot);
        return -ENOTDIR;
    }

    // the walk follows mounts, so landing on the root of one means this path is already covered
    spinlock_acquire(&mount_lock);
    bool covered = false;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (i != slot && mount_table[i].active && mount_table[i].fs_root == mp)
            covered = true;
    }
    spinlock_release(&mount_lock);

    if (covered || mp->mounted) {
        vfs_drop(mp);
        mount_slot_release(slot);
        serial_printf(LOG_ERROR "vfs_mount: %s is already mounted\n", path);
        return -EBUSY;
    }

    // Dispatch to the detected driver
    INode_t *fs_root = NULL;
    int r = -1;
    switch (fs_type) {
        case FS_TYPE_FAT32: r = fat32_mount(dev_inode, &fs_root); break;
        case FS_TYPE_EXT2:  r = ext2_mount (dev_inode, &fs_root); break;
        case FS_TYPE_EXFAT: r = exfat_mount(dev_inode, &fs_root); break;
        default: break;
    }

    if (r < 0 || !fs_root) {
        vfs_drop(mp);
        mount_slot_release(slot);
        serial_printf(LOG_ERROR "vfs_mount: driver mount failed for %s\n", path);
        return -EIO;
    }

    // the root takes the covered directory's place in the tree: same name, same parent.
    // its not put in the dentry cache, the covered directory keeps that slot
    strncpy(fs_root->name, mp->name, sizeof(fs_root->name) - 1);
    fs_root->name[sizeof(fs_root->name) - 1] = '\0';
    fs_root->parent   = mp->parent;
    vfs_hold(fs_root->parent);
    fs_root->attached = 1;

    spinlock_acquire(&mount_lock);
    strncpy(mount_table[slot].mount_path, path, PATH_MAX - 1);
    mount_table[slot].mount_path[PATH_MAX - 1] = '\0';
    mount_table[slot].mountpoint = mp;      // keeps the ref the lookup gave us
    mount_table[slot].fs_root    = fs_root;
    mount_table[slot].dev_inode  = dev_inode;
    mount_table[slot].fs_type    = fs_type;
    mp->mounted = fs_root;
    spinlock_release(&mount_lock);

    const char *fs_name = (fs_type == FS_TYPE_EXT2)  ? "ext2"  :
                          (fs_type == FS_TYPE_FAT32)  ? "fat32" :
                          (fs_type == FS_TYPE_EXFAT)  ? "exfat" : "unknown";
    serial_printf(LOG_OK "vfs_mount: mounted %s at %s\n", fs_name, path);
    return 0;
}

int vfs_umount(const char *path) {
    if (!path) return -EINVAL;

    spinlock_acquire(&mount_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active || !mount_table[i].fs_root ||
            strncmp(mount_table[i].mount_path, path, PATH_MAX) != 0)
            continue;

        INode_t *fs_root = mount_table[i].fs_root;
        INode_t *mp      = mount_table[i].mountpoint;

        // every inode holds its parent, so anything still in use inside the filesystem (an open
        // file, a cwd, another mount) shows up as an extra ref on the root. ours is the only one
        // when its safe to pull it out
        if (fs_root->shared > 1) {
            spinlock_release(&mount_lock);
            serial_printf(LOG_WARN "vfs_umount: %s is busy\n", path);
            return -EBUSY;
        }

        mp->mounted = NULL;
        mount_table[i].fs_root       = NULL;
        mount_table[i].mountpoint    = NULL;
        mount_table[i].dev_inode     = NULL;
        mount_table[i].mount_path[0] = '\0';
        mount_table[i].active        = false;
        spinlock_release(&mount_lock);

        vfs_drop(fs_root);
        vfs_drop(mp);
        serial_printf(LOG_OK "vfs_umount: unmounted %s\n", path);
        return 0;
    }
    spinlock_release(&mount_lock);
    serial_printf(LOG_ERROR "vfs_umount: %s is not mounted\n", path);
    return -EINVAL;
}
