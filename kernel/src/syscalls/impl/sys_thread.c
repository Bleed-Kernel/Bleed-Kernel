#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/errno.h>

long sys_gettid(void) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    return (long)task->id;
}

long sys_set_tid_address(uint64_t tidptr) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    task->clear_child_tid = tidptr;
    return (long)task->id;
}

// one thread, not the process it belongs to. tgid 0 is tkill and doesnt care which process
static long thread_kill(long tgid, long tid, int sig) {
    if (tid <= 0 || tgid < 0)
        return -EINVAL;
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;

    task_t *t = sched_get_task((uint64_t)tid);
    if (!t || (tgid && t->tgid != (uint64_t)tgid))
        return -ESRCH;
    if (t->state == TASK_FREE || t->state == TASK_DEAD || t->state == TASK_ZOMBIE)
        return -ESRCH;
    if (t->task_privilege == PRIVILEGE_KERNEL)
        return -EPERM;
    if (!sig)
        return 0;
    return signal_send(t, sig);
}

long sys_tkill(long tid, long sig) {
    return thread_kill(0, tid, (int)sig);
}

long sys_tgkill(long tgid, long tid, long sig) {
    if (tgid <= 0)
        return -EINVAL;
    return thread_kill(tgid, tid, (int)sig);
}
