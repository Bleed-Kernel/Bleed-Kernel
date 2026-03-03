#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>
#include <user/user_copy.h>

long sys_pipe(uint64_t user_fds_ptr) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (!user_fds_ptr || !user_ptr_valid(user_fds_ptr))
        return -EFAULT;

    int fds[2] = { -1, -1 };
    if (vfs_pipe(fds) != 0)
        return -EMFILE;

    if (copy_to_user(caller, (void *)user_fds_ptr, fds, sizeof(fds)) != 0) {
        vfs_close(fds[0]);
        vfs_close(fds[1]);
        return -EFAULT;
    }

    return 0;
}
