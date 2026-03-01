#include <fs/vfs.h>
#include <stdint.h>
#include <stdio.h>
#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <devices/type/fb_device.h>
#include <sched/scheduler.h>
#include <user/errno.h>

uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    task_t *caller = get_current_task();
    if (!caller)
        return (uint64_t)-ESRCH;
    if (fd >= MAX_FDS || !caller->fd_table)
        return (uint64_t)-EBADF;

    file_t* f = caller->fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops->ioctl)
        return (uint64_t)-ENOTTY;

    int result = f->inode->ops->ioctl(f->inode, request, (void*)arg);
    if (result < 0)
        return (uint64_t)-EINVAL;

    return (uint64_t)result;
}
