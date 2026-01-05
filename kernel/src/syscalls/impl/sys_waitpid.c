#include <stdint.h>
#include <sched/scheduler.h>

long sys_waitpid(uint64_t pid) {
    task_t *current = get_current_task();
    task_t *child = sched_get_task(pid);

    if (!child) return -1;
    if (child->parent != current) return -1;
    if (child->state == TASK_DEAD) {
        return 0;
    }

    current->state = TASK_BLOCKED;
    child->waiting_parent = current;

    sched_yield();
    return 0;
}
