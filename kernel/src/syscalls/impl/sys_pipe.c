#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>
#include <user/user_copy.h>

long sys_pipe(uint64_t user_fds_ptr, uint64_t flags) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (!user_fds_ptr || !user_ptr_valid(user_fds_ptr))
        return -EFAULT;

    if (flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;

    int fds[2] = { -1, -1 };
    int rc = vfs_pipe(fds);
    if (rc < 0)
        return rc;

    if (flags & O_CLOEXEC) {
        vfs_set_fd_flags(fds[0], FD_CLOEXEC);
        vfs_set_fd_flags(fds[1], FD_CLOEXEC);
    }
    if (flags & O_NONBLOCK) {
        vfs_set_nonblock(fds[0]);
        vfs_set_nonblock(fds[1]);
    }

    if (copy_to_user(caller, (void *)user_fds_ptr, fds, sizeof(fds)) != 0) {
        vfs_close(fds[0]);
        vfs_close(fds[1]);
        return -EFAULT;
    }

    return 0;
}
