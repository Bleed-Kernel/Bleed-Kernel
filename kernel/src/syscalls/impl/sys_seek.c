#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/errno.h>

long sys_seek(int fd, long offset, int whence) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    long r = vfs_seek(fd, offset, whence);
    if (r >= 0)
        return r;
    if (r == -2)
        return -EINVAL;
    return -EBADF;
}
