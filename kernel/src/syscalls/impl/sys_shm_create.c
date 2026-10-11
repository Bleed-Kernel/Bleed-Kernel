#include <ipc/shm.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_shm_create(uint64_t pages) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    return shm_create(task, pages);
}
