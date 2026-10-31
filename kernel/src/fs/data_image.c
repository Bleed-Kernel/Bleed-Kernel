/*
    the big programs (doom, quake2, half life) come as their own download, data.img, so the iso
    stays small and limine never has to load them.
*/
#include <fs/data_image.h>
#include <mm/spinlock.h>
#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <mm/kalloc.h>
#include <ACPI/acpi_ventoy.h>
#include <user/user_mount.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

#define DATA_FILE       "data.img"
#define DATA_MOUNT      "/mnt/data"
#define DATA_LABEL      "BLEEDDATA"
#define ISO_MOUNT       "/mnt/iso"      // an iso file with data.img inside, like on a ventoy stick
#define ISO_SEARCH_DEPTH 1              // the top of a drive and one folder down

#define EXT2_MAGIC_OFF  (1024 + 56)
#define EXT2_LABEL_OFF  (1024 + 120)
#define EXT2_MAGIC      0xEF53

// any old data.img wont do, it has to be ext2 with our label on it
static bool is_data_image(const char *file) {
    path_t   p     = vfs_path_from_abs(file);
    INode_t *inode = NULL;
    if (vfs_lookup(&p, &inode) < 0)
        return false;

    uint16_t magic = 0;
    char     label[16] = {0};
    bool ok = inode->type == INODE_FILE &&
              inode_read(inode, &magic, sizeof(magic), EXT2_MAGIC_OFF) == (long)sizeof(magic) &&
              magic == EXT2_MAGIC &&
              inode_read(inode, label, sizeof(label), EXT2_LABEL_OFF) == (long)sizeof(label) &&
              strncmp(label, DATA_LABEL, sizeof(label)) == 0;
    vfs_drop(inode);
    return ok;
}

// look in the top of every mount, false if none of them has one
static bool find_data_image(char *found, size_t size) {
    user_mount_info_t info;

    for (int i = 1; vfs_mount_info(i, &info) == 0; i++) {
        snprintf(found, size, "%s/" DATA_FILE, info.path);
        if (is_data_image(found))
            return true;
    }
    return false;
}

static bool data_mounted(void) {
    user_mount_info_t info;

    for (int i = 1; vfs_mount_info(i, &info) == 0; i++) {
        if (!strcmp(info.path, DATA_MOUNT))
            return true;
    }
    return false;
}

static bool name_is_iso(const char *name) {
    size_t len = strlen(name);
    if (len < 5 || name[0] == '.')
        return false;
    const char *ext = name + len - 4;
    return ext[0] == '.' && (ext[1] | 0x20) == 'i' && (ext[2] | 0x20) == 's' && (ext[3] | 0x20) == 'o';
}

static void mount_data_image(const char *file) {
    int r = vfs_mount_file(DATA_MOUNT, file);
    if (r == 0)
        serial_printf(LOG_OK "data_image: %s mounted at " DATA_MOUNT "\n", file);
    else
        serial_printf(LOG_WARN "data_image: couldnt mount %s at " DATA_MOUNT " (%d)\n", file, r);
}

// mount the iso at ISO_MOUNT and keep it if it has data.img in it
static bool try_iso(const char *iso, char *scratch, size_t size) {
    if (vfs_mount_file(ISO_MOUNT, iso) != 0)
        return false;

    snprintf(scratch, size, ISO_MOUNT "/" DATA_FILE);
    if (is_data_image(scratch)) {
        serial_printf(LOG_OK "data_image: %s has " DATA_FILE " in it, mounted at " ISO_MOUNT "\n", iso);
        mount_data_image(scratch);
        return true;
    }
    vfs_umount(ISO_MOUNT);
    return false;
}

// every .iso in dir, and one folder down when depth allows. ventoy sticks keep them like this
static bool search_isos(const char *dir, int depth, char *path, char *scratch, size_t size) {
    path_t   p    = vfs_path_from_abs(dir);
    INode_t *node = NULL;
    if (vfs_lookup(&p, &node) < 0)
        return false;

    bool found = false;
    for (size_t i = 0; !found; i++) {
        INode_t *child = NULL;
        if (vfs_readdir(node, i, &child) < 0 || !child)
            break;

        snprintf(path, size, "%s/%s", dir, child->name);
        if (child->type == INODE_FILE && name_is_iso(child->name)) {
            serial_printf(LOG_INFO "data_image: looking in %s\n", path);
            found = try_iso(path, scratch, size);
        } else if (child->type == INODE_DIRECTORY && depth > 0 && child->name[0] != '.' && !child->mounted) {
            char *sub = kmalloc(size);
            if (sub) {
                strcpy(sub, path);
                found = search_isos(sub, depth - 1, path, scratch, size);
                kfree(sub);
            }
        }
        vfs_drop(child);
    }
    vfs_drop(node);
    return found;
}

static bool find_in_isos(char *path, char *scratch, size_t size) {
    user_mount_info_t info;
    const char       *booted = ventoy_image_path();

    // ventoy said which iso it booted, look for that one before guessing
    for (int i = 1; booted && vfs_mount_info(i, &info) == 0; i++) {
        if (!strcmp(info.fs, "iso9660"))
            continue;
        snprintf(path, size, "%s%s%s", info.path, booted[0] == '/' ? "" : "/", booted);
        if (try_iso(path, scratch, size))
            return true;
    }

    for (int i = 1; vfs_mount_info(i, &info) == 0; i++) {
        // the iso we just tried and the data image itself never hold one
        if (!strcmp(info.path, ISO_MOUNT) || !strcmp(info.path, DATA_MOUNT) || !strcmp(info.fs, "iso9660"))
            continue;
        if (search_isos(info.path, ISO_SEARCH_DEPTH, path, scratch, size))
            return true;
    }
    return false;
}

static void probe_data_image(void) {
    if (data_mounted())
        return;     // one is in already

    // PATH_MAX sized
    char *path    = kmalloc(PATH_MAX);
    char *scratch = kmalloc(PATH_MAX);
    if (!path || !scratch) {
        kfree(path);
        kfree(scratch);
        return;
    }

    if (find_data_image(path, PATH_MAX))
        mount_data_image(path);
    else if (!find_in_isos(path, scratch, PATH_MAX))
        serial_printf(LOG_WARN "data_image: no " DATA_FILE " on anything mounted\n");

    kfree(path);
    kfree(scratch);
}

void data_image_scan(void) {
    // usb_service calls this from a kernel task with interrupts on
    unsigned long flags = irq_push();
    probe_data_image();
    irq_restore(flags);
}
