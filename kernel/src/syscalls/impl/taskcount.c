#include <sched/scheduler.h>
#include <status.h>
#include <user/user_copy.h>
#include <stdio.h>
#include <stdint.h>

int sys_taskcount(uint64_t *user_count) {
    if (!user_count)
        return -1;

    uint64_t count = get_task_count();

    if (copy_to_user(user_count, &count, sizeof(uint64_t)) != 0)
        return 0;

    return 0;
}
