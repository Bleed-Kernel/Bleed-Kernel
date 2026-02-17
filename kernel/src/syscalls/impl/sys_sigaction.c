#include <syscalls/syscall.h>
#include <sched/signal.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

long sys_sigaction(int sig, const sigaction_t *user_act, sigaction_t *user_old) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    sigaction_t kact;
    sigaction_t kold;
    sigaction_t *act_ptr = NULL;
    sigaction_t *old_ptr = NULL;

    if (user_act) {
        if (copy_from_user(task, &kact, user_act, sizeof(kact)) != 0)
            return -EFAULT;
        act_ptr = &kact;
    }

    if (user_old)
        old_ptr = &kold;

    long rc = signal_set_action(task, sig, act_ptr, old_ptr);
    if (rc < 0)
        return rc;

    if (user_old) {
        if (copy_to_user(task, user_old, &kold, sizeof(kold)) != 0)
            return -EFAULT;
    }

    return 0;
}
