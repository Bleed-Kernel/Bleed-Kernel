#include <devices/devices.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <drivers/framebuffer/framebuffer.h>
#include <devices/type/fb_device.h>
#include <mm/paging.h>
#include <stdio.h>
#include <string.h>
#include <sched/scheduler.h>

static fb_device_t *fb0 = NULL;

static long fb_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    fb_device_t *fb = inode->internal_data;
    spinlock_acquire(&fb->lock);

    uint8_t *fb_ptr = (uint8_t*)fb->fb_phys;

    if (offset >= fb->height * fb->pitch) {
        spinlock_release(&fb->lock);
        return 0;
    }

    size_t max_write = fb->height * fb->pitch - offset;
    size_t write_len = len < max_write ? len : max_write;

    memcpy(fb_ptr + offset, buf, write_len);

    spinlock_release(&fb->lock);
    return write_len;
}

static long fb_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    fb_device_t *fb = inode->internal_data;
    spinlock_acquire(&fb->lock);

    uint8_t *fb_ptr = (uint8_t*)fb->fb_phys;

    if (offset >= fb->height * fb->pitch) {
        spinlock_release(&fb->lock);
        return 0;
    }

    size_t max_read = fb->height * fb->pitch - offset;
    size_t read_len = len < max_read ? len : max_read;

    memcpy(buf, fb_ptr + offset, read_len);

    spinlock_release(&fb->lock);
    return read_len;
}

static int fb_ioctl(INode_t *inode, unsigned long request, void *arg) {
    fb_device_t *fb = inode->internal_data;
    
    switch (request) {
        case FB_IOC_GET_INFO: {
            struct fb_info info = {
                .width  = fb->width,
                .height = fb->height,
                .pitch  = fb->pitch,
                .bpp    = 32
            };
            memcpy(arg, &info, sizeof(struct fb_info));
            return 0;
        }
        default:
            return -1;
    }
}

static struct INodeOps fb_inode_ops = {
    .write = fb_write,
    .read  = fb_read,
    .ioctl = fb_ioctl,
};

void fb_device_init() {
    fb0 = kmalloc(sizeof(fb_device_t));
    memset(fb0, 0, sizeof(*fb0));
    spinlock_init(&fb0->lock);

    fb0->fb_phys = (uintptr_t)framebuffer_get_addr(0);
    fb0->width   = framebuffer_get_width(0);
    fb0->height  = framebuffer_get_height(0);
    fb0->pitch   = framebuffer_get_pitch(0);

    fb0->device.ops = &fb_inode_ops;
    fb0->device.internal_data = fb0;
    fb0->device.type = INODE_FILE;

    file_t* fb_fd = kmalloc(sizeof(file_t));
    fb_fd->inode = &fb0->device;
    fb_fd->flags = O_RDWR;
    fb_fd->offset = 0;
    fb_fd->shared = 1;

    current_fd_table->fds[4] = fb_fd;

    device_register(&fb0->device, "fb0");
}
