/*
    the big programs (doom, quake2, half life) come as their own download, data.img, so the iso
    stays small and limine never has to load them.
*/
#include <fs/data_image.h>
#include <mm/spinlock.h>
#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <user/user_mount.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

#define DATA_FILE       "data.img"
#define DATA_MOUNT      "/mnt/data"
#define DATA_LABEL      "BLEEDDATA"

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

static void probe_data_image(void) {
    user_mount_info_t info;
    char              found[sizeof(info.path) + sizeof(DATA_FILE) + 1] = "";

    for (int i = 1; vfs_mount_info(i, &info) == 0; i++) {
        if (!strcmp(info.path, DATA_MOUNT))
            return;     // one is in already
        if (found[0])
            continue;

        char file[sizeof(found)];
        snprintf(file, sizeof(file), "%s/" DATA_FILE, info.path);
        if (is_data_image(file))
            strcpy(found, file);
    }
    if (!found[0])
        return;

    int r = vfs_mount_file(DATA_MOUNT, found);
    if (r == 0)
        serial_printf(LOG_OK "data_image: %s mounted at " DATA_MOUNT "\n", found);
    else
        serial_printf(LOG_WARN "data_image: couldnt mount %s at " DATA_MOUNT " (%d)\n", found, r);
}

void data_image_scan(void) {
    // usb_service calls this from a kernel task with interrupts on
    unsigned long flags = irq_push();
    probe_data_image();
    irq_restore(flags);
}
