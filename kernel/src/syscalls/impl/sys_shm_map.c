#include <ipc/shm.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_shm_map(uint64_t id, uint64_t user_pages_ptr) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    return shm_map(task, id, user_pages_ptr);
}
