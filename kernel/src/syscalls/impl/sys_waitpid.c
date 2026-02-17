#include <stdint.h>
#include <sched/scheduler.h>
#include <user/errno.h>

long sys_waitpid(uint64_t pid) {
    task_t *current = get_current_task();
    if (!current)
        return -ESRCH;

    task_t *child = sched_get_task(pid);

    if (!child)
        return -ESRCH;

    for (;;) {
        if (child->state == TASK_DEAD) {
            return 0;
        }

        current->state = TASK_BLOCKED;
        current->wait_next = child->wait_queue;
        child->wait_queue = current;

        sched_yield();
    }
}
