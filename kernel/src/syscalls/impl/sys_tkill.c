#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/signal.h>
#include <user/errno.h>

long sys_tkill(long pid, signal_number_t signal) {
    if (pid <= 0)
        return -EINVAL;

    task_t *target = sched_get_task((uint64_t)pid);
    if (!target)
        return -ESRCH;

    if (target->task_privilege == PRIVILEGE_KERNEL)
        return -EPERM;

    return signal_send(target, signal);
}