#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len) {
    task_t *caller = get_current_task();
    if (!caller)
        return (uint64_t)-ESRCH;
    if (fd >= MAX_FDS || !caller->fd_table)
        return (uint64_t)-EBADF;

    file_t *f = caller->fd_table->fds[fd];
    if (!f)
        return (uint64_t)-EBADF;

    if (len == 0)
        return 0;
    if (!user_buf)
        return (uint64_t)-EFAULT;

    char kbuf[2048];
    uint64_t copied = 0;

    while (copied < len) {
        size_t batch_size = len - copied;
        if (batch_size > sizeof(kbuf))
            batch_size = sizeof(kbuf);

        long r = vfs_read((int)fd, kbuf, batch_size);
        if (r < 0) {
            if (copied)
                return copied;
            if (r == -3 || r == -2 || r == -1)
                return (uint64_t)-EBADF;
            if (r == -EAGAIN)
                return (uint64_t)-EAGAIN;
            if (r == -EINTR)
                return (uint64_t)-EINTR;
            return (uint64_t)-EIO;
        }
        if (r == 0)
            break;

        if (copy_to_user(caller, (void *)(user_buf + copied), kbuf, (size_t)r) != 0)
            return copied ? copied : (uint64_t)-EFAULT;
        copied += (uint64_t)r;

        if ((size_t)r < batch_size)
            break;
    }

    return copied;
}
