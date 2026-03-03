#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_dup2(uint64_t oldfd, uint64_t newfd) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS)
        return -EBADF;

    int rc = vfs_dup2((int)oldfd, (int)newfd);
    if (rc < 0)
        return -EBADF;
    return rc;
}
