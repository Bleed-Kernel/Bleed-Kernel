#include <boot/sysinfo/sysinfo.h>
#include <user/user_copy.h>
#include <mm/kalloc.h>
#include <sched/scheduler.h>

int sys_meminfo(system_memory_info_t *user_buf) {
    if (!user_buf) return -1;

    system_memory_info_t *info = get_system_memory_info();
    if (!info) return -1;

    task_t *caller = get_current_task();
    if (copy_to_user(caller, user_buf, info, sizeof(system_memory_info_t)) != 0) {
        kfree(info, sizeof(system_memory_info_t));
        return -1;
    }

    kfree(info, sizeof(system_memory_info_t));
    return 0;
}
