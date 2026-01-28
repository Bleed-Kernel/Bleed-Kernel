#include <sched/scheduler.h>
#include <user/user_task.h>
#include <status.h>
#include <string.h>
#include <user/user_copy.h>
#include <stdio.h>

int sys_taskinfo(uint64_t pid, user_task_info_t *user_info) {
    if (!user_info)
        return -1;

    task_t *task = sched_get_task(pid);
    if (!task)
        return -1;

    user_task_info_t info;
    memset(&info, 0, sizeof(info));

    info.id = task->id;
    info.state = task->state;
    info.quantum_remaining = task->quantum_remaining;
    strncpy(info.name, task->name, 128-1);
    info.name[128-1] = '\0';

    return copy_to_user(user_info, &info, sizeof(info)) == 0 ? 0 : -1;

    return 0;
}
