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

long sys_sigaltstack(const user_stack_t *user_ss, user_stack_t *user_old) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    user_stack_t ss, old;
    if (user_ss && copy_from_user(task, &ss, user_ss, sizeof(ss)) != 0)
        return -EFAULT;

    uintptr_t sp = task->context ? task->context->rsp : 0;
    long rc = signal_set_altstack(task, user_ss ? &ss : NULL, user_old ? &old : NULL, sp);
    if (rc < 0)
        return rc;

    if (user_old && copy_to_user(task, user_old, &old, sizeof(old)) != 0)
        return -EFAULT;
    return 0;
}
