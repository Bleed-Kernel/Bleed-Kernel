#include <stdint.h>
#include <sched/scheduler.h>

uint64_t sys_taskcount(void) {
    return get_task_count();
}