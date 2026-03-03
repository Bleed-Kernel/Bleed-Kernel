#include <ipc/zero_copy.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_ipc_recv(uint64_t user_msg_ptr) {
    task_t *receiver = get_current_task();
    if (!receiver)
        return -ESRCH;

    return ipc_recv(receiver, user_msg_ptr);
}

