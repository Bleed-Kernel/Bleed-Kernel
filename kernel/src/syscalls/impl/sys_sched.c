#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <cpu/percpu.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <string.h>

// every task can run on every cpu thats up, so thats the mask
long sys_sched_getaffinity(uint64_t pid, uint64_t len, uint8_t *user_mask) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    if (pid && !sched_get_task(pid))
        return -ESRCH;

    uint8_t mask[MAX_CPUS / 8 + 8];
    size_t  bytes = ((MAX_CPUS + 63) / 64) * 8;
    if (len < bytes)
        return -EINVAL;

    memset(mask, 0, sizeof(mask));
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        if (cpus[i].online == 1)
            mask[i / 8] |= (uint8_t)(1u << (i % 8));
    }

    if (copy_to_user(task, user_mask, mask, bytes) != 0)
        return -EFAULT;
    return (long)bytes;
}
