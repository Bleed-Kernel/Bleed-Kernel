#include <devices/devices.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <devices/type/fb_device.h>
#include <devices/type/tty_device.h>
#include <sched/scheduler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_copy.h>

static fb_device_t  *fb0;
static uint8_t      *fb_backbuffer;
static uint64_t     fb_owner_pid;   // task that took the screen with FB_IOC_ACQUIRE
static uint64_t     fb_hold_pid;

// ideally this become more flexable soon?

static long fb_write(INode_t *inode, const void *buf, size_t len, size_t offset) {
    fb_device_t *fb = inode->internal_data;
    if (!fb_backbuffer) return 0;

    size_t fb_size = fb->height * fb->pitch;
    if (offset >= fb_size) return 0;
    if (len > fb_size - offset)
        len = fb_size - offset;

    // buf is already a kernel copy by the time it gets here
    memcpy(fb_backbuffer + offset, buf, len);
    return len;
}

static long fb_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    fb_device_t *fb = inode->internal_data;
    if (!fb_backbuffer) return 0;

    size_t fb_size = fb->height * fb->pitch;
    if (offset >= fb_size) return 0;
    if (len > fb_size - offset)
        len = fb_size - offset;

    memcpy(buf, fb_backbuffer + offset, len);
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
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    // one owner at a time, it always gets cleared on exit so a set pid is a live task
    if (request == FB_IOC_ACQUIRE) {
        if (fb_owner_pid && fb_owner_pid != task->id)
            return -EBUSY;
        fb_owner_pid = task->id;
        if (fb_hold_pid == task->id) fb_hold_pid = 0;
        tty_set_graphics(1);
        return 0;
    }

    if (request == FB_IOC_RELEASE) {
        if (fb_owner_pid != task->id)
            return -EPERM;
        fb_owner_pid = 0;
        if (!fb_hold_pid) tty_set_graphics(0);
        sched_wake_event_waiters();
        return 0;
    }

    if (!arg)
        return -EFAULT;

    if (request == FB_IOC_GET_OWNER)
        return copy_to_user(task, arg, &fb_owner_pid, sizeof(fb_owner_pid)) == 0 ? 0 : -EFAULT;

    if (request == FB_IOC_GET_INFO) {
        struct fb_info info = {
            .width  = fb->width,
            .height = fb->height,
            .pitch  = fb->pitch,
            .bpp    = fb->bpp,
        };
        return copy_to_user(task, arg, &info, sizeof(info)) == 0 ? 0 : -EFAULT;
    }

    if (request == FB_IOC_FLIP) {
        size_t size = fb->height * fb->pitch;
        return copy_from_user(task, (void*)fb->fb_phys, arg, size) == 0 ? 0 : -EFAULT;
    }

    return -EINVAL;
}

static struct INodeOps fb_inode_ops = {
    .write = fb_write,
    .read  = fb_read,
    .size  = fb_size,
    .ioctl = fb_ioctl,
};

void fb_device_task_exit(task_t *task) {
    if (!task) return;
    bool was_holding = (fb_owner_pid && fb_owner_pid == task->id) ||
                       (fb_hold_pid && fb_hold_pid == task->id);
    if (!was_holding) return;

    if (fb_owner_pid == task->id) fb_owner_pid = 0;
    if (fb_hold_pid == task->id)  fb_hold_pid  = 0;
    if (!fb_owner_pid && !fb_hold_pid) tty_set_graphics(0);
    sched_wake_event_waiters();
}

uint64_t fb_device_screen_pid(void) {
    return fb_owner_pid ? fb_owner_pid : fb_hold_pid;
}

void fb_device_hold_until(task_t *task) {
    // it may have already died before we got here, the exit hook wouldve missed it
    if (!task || task->state == TASK_ZOMBIE || task->state == TASK_DEAD || task->state == TASK_FREE) {
        fb_hold_pid = 0;
        if (!fb_owner_pid) tty_set_graphics(0);
        return;
    }
    fb_hold_pid = task->id;
}

void fb_device_init(void) {
    fb0 = kmalloc(sizeof(fb_device_t));
    if (!fb0) return;
    spinlock_init(&fb0->lock);

    fb0->fb_phys    = (uintptr_t)framebuffer_get_addr(0);
    fb0->width      = framebuffer_get_width(0);
    fb0->height     = framebuffer_get_height(0);
    fb0->pitch      = framebuffer_get_pitch(0);
    fb0->bpp        = framebuffer_get_bpp(0);

    size_t size = fb0->height * fb0->pitch;
    fb_backbuffer = kmalloc(size);
    if (!fb_backbuffer) return;
    memset(fb_backbuffer, 0, size);

    // Allocate inode for VFS / Devices
    INode_t* dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode) return;
    memset(dev_inode, 0, sizeof(INode_t));

    dev_inode->ops = &fb_inode_ops;
    dev_inode->internal_data = fb0;
    dev_inode->type = INODE_DEVICE;
    dev_inode->shared = 1;
    device_register(dev_inode, "fb0");
}
