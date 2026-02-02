#include <stdint.h>
#include <string.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/user_task.h>

uint64_t sys_taskinfo(uint64_t pid, uint64_t user_info_ptr) {
    if (!user_info_ptr) return -1;

    task_t *task = sched_get_task(pid);
    if (!task) {
        return -1;
    }

    user_task_info_t info;
    info.id = task->id;
    info.state = task->state;
    info.quantum_remaining = task->quantum_remaining;
    info.privilege_level = task->task_privilege;
    
    strncpy(info.name, task->name, sizeof(info.name) - 1);
    info.name[sizeof(info.name) - 1] = '\0';

    task_t *caller = get_current_task();
    if (copy_to_user(caller, (void *)user_info_ptr, &info, sizeof(user_task_info_t)) != 0) {
        return -1;
    }

    return 0;
}