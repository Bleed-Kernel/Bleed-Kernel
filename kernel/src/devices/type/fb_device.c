#include <devices/devices.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <devices/type/fb_device.h>
#include <sched/scheduler.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static fb_device_t  *fb0;
static uint8_t      *fb_backbuffer;

// ideally this become more flexable soon?

static long fb_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    fb_device_t *fb = inode->internal_data;

    size_t fb_size = fb->height * fb->pitch;
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size)
        len = fb_size - offset;

    umemcpy(fb_backbuffer + offset, buf, len);
    return len;
}

static long fb_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    fb_device_t *fb = inode->internal_data;

    size_t fb_size = fb->height * fb->pitch;
    if (offset >= fb_size) return 0;
    if (offset + len > fb_size)
        len = fb_size - offset;

    umemcpy(buf, fb_backbuffer + offset, len);
    return len;
}

static size_t fb_size(INode_t *inode) {
    fb_device_t *fb = inode->internal_data;
    if (!fb)
        return 0;
    return fb->height * fb->pitch;
}

static int fb_ioctl(INode_t *inode, unsigned long request, void *arg) {
    (void)inode;// oh yeah its big brain time
    fb_device_t *fb = fb0;
    if (request == FB_IOC_GET_INFO) {
        struct fb_info info = {
            .width  = fb->width,
            .height = fb->height,
            .pitch  = fb->pitch,
            .bpp    = fb->bpp,
        };
        umemcpy(arg, &info, sizeof(info));
        return 0;
    }

    if (request == FB_IOC_FLIP) {
        size_t size = fb->height * fb->pitch;
        umemcpy((void*)fb->fb_phys, arg, size);
        return 0;
    }

    return -1;
}

static struct INodeOps fb_inode_ops = {
    .write = fb_write,
    .read  = fb_read,
    .size  = fb_size,
    .ioctl = fb_ioctl,
};

void fb_device_init(void) {
    fb0 = kmalloc(sizeof(fb_device_t));
    spinlock_init(&fb0->lock);

    fb0->fb_phys    = (uintptr_t)framebuffer_get_addr(0);
    fb0->width      = framebuffer_get_width(0);
    fb0->height     = framebuffer_get_height(0);
    fb0->pitch      = framebuffer_get_pitch(0);
    fb0->bpp        = framebuffer_get_bpp(0);

    size_t size = fb0->height * fb0->pitch;
    fb_backbuffer = kmalloc(size);
    memset(fb_backbuffer, 0, size);

    // Allocate inode for VFS / Devices
    INode_t* dev_inode = kmalloc(sizeof(INode_t));
    memset(dev_inode, 0, sizeof(INode_t));

    dev_inode->ops = &fb_inode_ops;
    dev_inode->internal_data = fb0;
    dev_inode->type = INODE_FILE;
    dev_inode->shared = 1;
    device_register(dev_inode, "fb0");
}
