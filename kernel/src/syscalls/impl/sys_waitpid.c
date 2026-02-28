#include <stdint.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <user/signal.h>

long sys_waitpid(uint64_t pid) {
    if (pid == 0)
        return -EINVAL;

    task_t *current = get_current_task();
    if (!current)
        return -ESRCH;

    current->wait_target_pid = pid;

    task_t *child = sched_get_task(pid);

    if (!child) {
        current->wait_target_pid = 0;
        return -ESRCH;
    }
    if (child->ppid != current->id) {
        current->wait_target_pid = 0;
        return -ECHILD;
    }

    for (;;) {
        child = sched_get_task(pid);
        if (!child) {
            current->wait_target_pid = 0;
            return -ECHILD;
        }
        if (child->ppid != current->id) {
            current->wait_target_pid = 0;
            return -ECHILD;
        }

        if (child->state == TASK_ZOMBIE) {
            uint64_t child_pid = child->id;
            current->wait_target_pid = 0;
            sched_mark_task_dead(child);
            return (long)child_pid;
        }

        sigset_t ready = current->sig_pending & ~current->sig_blocked;
        sigset_t sigchld_bit = (sigset_t)1ULL << (SIGCHLD - 1);
        if (ready & ~sigchld_bit) {
            current->wait_target_pid = 0;
            return -EINTR;
        }

        current->state = TASK_BLOCKED;
        sched_yield();
    }
}
