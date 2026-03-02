#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <drivers/serial/serial.h>

long sys_fork(void) {
    task_t *parent = get_current_task();
    if (!parent)
        return -ESRCH;

    task_t *child = sched_fork_from_context(parent->context);
    if (!child) {
        return -ENOMEM;
    }

    return (long)child->id;
}
