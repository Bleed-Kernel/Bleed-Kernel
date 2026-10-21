#include <ipc/shm.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_shm_map_at(uint64_t id, uint64_t first, uint64_t pages, uint64_t addr) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    return shm_map_at(task, id, first, pages, addr);
}