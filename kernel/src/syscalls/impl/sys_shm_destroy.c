#include <ipc/shm.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_shm_destroy(uint64_t id) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    return shm_destroy(task, id);
}
