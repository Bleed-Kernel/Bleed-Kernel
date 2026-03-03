#include <ipc/zero_copy.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_ipc_send(uint64_t target_pid, uint64_t src_addr, uint64_t pages) {
    task_t *sender = get_current_task();
    if (!sender)
        return -ESRCH;

    return ipc_send_pages(sender, target_pid, src_addr, pages);
}

