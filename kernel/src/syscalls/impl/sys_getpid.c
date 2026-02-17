#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/errno.h>

long sys_getpid(void) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    return (long)task->id;
}
