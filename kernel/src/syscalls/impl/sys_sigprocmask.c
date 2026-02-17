#include <syscalls/syscall.h>
#include <sched/signal.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

long sys_sigprocmask(int how, const sigset_t *user_set, sigset_t *user_old) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    sigset_t kset;
    sigset_t kold;
    sigset_t *set_ptr = NULL;
    sigset_t *old_ptr = NULL;

    if (user_set) {
        if (copy_from_user(task, &kset, user_set, sizeof(kset)) != 0)
            return -EFAULT;
        set_ptr = &kset;
    }

    if (user_old)
        old_ptr = &kold;

    long rc = signal_set_mask(task, how, set_ptr, old_ptr);
    if (rc < 0)
        return rc;

    if (user_old) {
        if (copy_to_user(task, user_old, &kold, sizeof(kold)) != 0)
            return -EFAULT;
    }

    return 0;
}
