#include <devices/devices.h>
#include <kmd/kmd_loader.h>
#include <drivers/serial/serial.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

static long kmd_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode;
    if (!buf || !len)
        return 0;

    char  *text = kmalloc(KMD_TEXT_MAX);
    if (!text)
        return -ENOMEM;
    size_t size = kmd_describe(text, KMD_TEXT_MAX);

    if (offset >= size) {
        kfree(text);
        return 0;
    }
    if (len > size - offset)
        len = size - offset;
    memcpy(buf, text + offset, len);
    kfree(text);
    return (long)len;
}

static size_t kmd_size(INode_t *inode) {
    (void)inode;
    char *text = kmalloc(KMD_TEXT_MAX);
    if (!text)
        return 0;
    size_t size = kmd_describe(text, KMD_TEXT_MAX);
    kfree(text);
    return size;
}

static char   kmd_line[64];
static size_t kmd_line_len = 0;

static int kmd_command(char *cmd) {
    size_t end = strlen(cmd);
    while (end && (cmd[end - 1] == ' ' || cmd[end - 1] == '\r'))
        cmd[--end] = 0;
    if (!end)
        return 0;

    if (strncmp(cmd, "unload ", 7) != 0) {
        serial_printf(LOG_WARN "kmd: /dev/kmd doesnt know '%s', only unload NAME\n", cmd);
        return -EINVAL;
    }

    const char *name = cmd + 7;
    while (*name == ' ')
        name++;

    serial_printf(LOG_INFO "kmd: asked to unload %s\n", name);
    return kmd_unload(name);
}

static long kmd_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    (void)inode; (void)offset;
    if (!buf)
        return -EINVAL;

    int r = 0;
    const char *in = buf;
    for (size_t i = 0; i < len; i++) {
        if (in[i] != '\n') {
            if (kmd_line_len < sizeof(kmd_line) - 1)
                kmd_line[kmd_line_len++] = in[i];
            continue;
        }

        kmd_line[kmd_line_len] = 0;
        kmd_line_len = 0;
        r = kmd_command(kmd_line);
    }
    return r < 0 ? r : (long)len;
}

static INodeOps_t kmd_inode_ops = {
    .read  = kmd_read,
    .write = kmd_write,
    .size  = kmd_size,
};

void kmd_device_init(void) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops    = &kmd_inode_ops;
    dev_inode->type   = INODE_DEVICE;
    dev_inode->shared = 1;

    device_register(dev_inode, "kmd");
}
