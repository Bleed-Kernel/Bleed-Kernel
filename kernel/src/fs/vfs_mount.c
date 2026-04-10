#include <fs/vfs_mount.h>
#include <fs/vfs.h>
#include <fs/fat32/fat32.h>
#include <fs/ext2/ext2.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>

#define PROBE_FAT32_ROOT_ENTRY_OFF  17    //root entry count
#define PROBE_FAT32_SIG_OFF         510   // 0x55 0xAA boot signature
#define PROBE_EXT2_MAGIC_OFF        (1024 + 56)
#define PROBE_EXT2_MAGIC            0xEF53

typedef struct {
    char      mount_path[PATH_MAX];
    INode_t  *fs_root;
    INode_t  *dev_inode;
    fs_type_t fs_type;
    bool      active;
} mount_entry_t;

static mount_entry_t mount_table[VFS_MAX_MOUNTS];
static spinlock_t    mount_lock = {0};

INode_t *vfs_mount_resolve(INode_t *inode) {
    if (!inode || inode->name[0] == '\0') return NULL;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mount_table[i].active) continue;

        const char *p    = mount_table[i].mount_path;
        const char *base = p;
        for (const char *q = p; *q; q++)
            if (*q == '/') base = q + 1;

        if (base[0] != '\0' && strcmp(base, inode->name) == 0)
            return mount_table[i].fs_root;
    }
    return NULL;
}

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

    // FAT32 Probe sector 0 must end in 0x55 0xAA
    uint8_t sector0[512];
    if (inode_read(dev_inode, sector0, sizeof(sector0), 0) == (long)sizeof(sector0)) {
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
    mount_table[slot].active = true;
    spinlock_release(&mount_lock);

    fs_type_t fs_type = vfs_detect_fs(dev_inode);
    if (fs_type == FS_TYPE_UNKNOWN) {
        spinlock_acquire(&mount_lock);
        mount_table[slot].active = false;
        spinlock_release(&mount_lock);
        serial_printf(LOG_ERROR "vfs_mount: Unsupported filesystem\n");
        return -1;
    }

    path_t p = vfs_path_from_abs(path);
    INode_t *mp = NULL;
    if (vfs_lookup(&p, &mp) < 0) {
        INode_t *created = NULL;
        if (vfs_create(&p, &created, INODE_DIRECTORY) < 0) {
            spinlock_acquire(&mount_lock);
            mount_table[slot].active = false;
            spinlock_release(&mount_lock);
            serial_printf(LOG_ERROR "vfs_mount: could not create mount point %s\n", path);
            return -1;
        }
        vfs_drop(created);
        if (vfs_lookup(&p, &mp) < 0) {
            spinlock_acquire(&mount_lock);
            mount_table[slot].active = false;
            spinlock_release(&mount_lock);
            serial_printf(LOG_ERROR "vfs_mount: could not look up mount point %s\n", path);
            return -1;
        }
    }

    spinlock_acquire(&mount_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (i == slot) continue;
        if (mount_table[i].active &&
            strncmp(mount_table[i].mount_path, path, PATH_MAX) == 0) {
            spinlock_release(&mount_lock);
            vfs_drop(mp);
            spinlock_acquire(&mount_lock);
            mount_table[slot].active = false;
            spinlock_release(&mount_lock);
            serial_printf(LOG_ERROR "vfs_mount: %s is already mounted\n", path);
            return -1;
        }
    }
    spinlock_release(&mount_lock);

    // Dispatch to the detected driver
    INode_t *fs_root = NULL;
    int r = -1;
    switch (fs_type) {
        case FS_TYPE_FAT32: r = fat32_mount(dev_inode, &fs_root); break;
        case FS_TYPE_EXT2:  r = ext2_mount (dev_inode, &fs_root); break;
        default: break;
    }

    if (r < 0) {
        vfs_drop(mp);
        spinlock_acquire(&mount_lock);
        mount_table[slot].active = false;
        spinlock_release(&mount_lock);
        serial_printf(LOG_ERROR "vfs_mount: driver mount failed for %s\n", path);
        return -1;
    }

    fs_root->parent = mp->parent;

    vfs_drop(mp);

    spinlock_acquire(&mount_lock);
    strncpy(mount_table[slot].mount_path, path, PATH_MAX - 1);
    mount_table[slot].mount_path[PATH_MAX - 1] = '\0';
    mount_table[slot].fs_root   = fs_root;
    mount_table[slot].dev_inode = dev_inode;
    mount_table[slot].fs_type   = fs_type;
    spinlock_release(&mount_lock);

    const char *fs_name = (fs_type == FS_TYPE_EXT2)  ? "ext2"  :
                          (fs_type == FS_TYPE_FAT32)  ? "fat32" : "unknown";
    serial_printf(LOG_OK "vfs_mount: mounted %s at %s\n", fs_name, path);
    return 0;
}

int vfs_umount(const char *path) {
    if (!path) return -1;

    spinlock_acquire(&mount_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mount_table[i].active &&
            strncmp(mount_table[i].mount_path, path, PATH_MAX) == 0) {
            INode_t *fs_root = mount_table[i].fs_root;
            mount_table[i].fs_root     = NULL;
            mount_table[i].dev_inode   = NULL;
            mount_table[i].mount_path[0] = '\0';
            mount_table[i].active      = false;
            spinlock_release(&mount_lock);
            vfs_drop(fs_root);
            serial_printf(LOG_OK "vfs_umount: unmounted %s\n", path);
            return 0;
        }
    }
    spinlock_release(&mount_lock);
    serial_printf(LOG_ERROR "vfs_umount: %s is not mounted\n", path);
    return -1;
}