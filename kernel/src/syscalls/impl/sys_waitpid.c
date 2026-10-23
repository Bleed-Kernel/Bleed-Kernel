#include <stdint.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <user/signal.h>
#include <mm/spinlock.h>
#include <user/user_copy.h>

static long wait_child(uint64_t pid, int *status) {
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
        // the check and the block have to be one step, a child exiting in between would
        // send its SIGCHLD before we are BLOCKED and nothing would ever wake us
        unsigned long flags = irq_push();

        child = sched_get_task(pid);
        if (!child || child->ppid != current->id) {
            current->wait_target_pid = 0;
            irq_restore(flags);
            return -ECHILD;
        }

        if (child->state == TASK_ZOMBIE) {
            uint64_t child_pid = child->id;
            *status = child->exit_signal ? (child->exit_signal & 0x7F) : (child->exit_code & 0xFF) << 8;
            current->wait_target_pid = 0;
            irq_restore(flags);
            sched_mark_task_dead(child);
            return (long)child_pid;
        }

        sigset_t ready = current->sig_pending & ~current->sig_blocked;
        sigset_t sigchld_bit = (sigset_t)1ULL << (SIGCHLD - 1);

        // a window changing size is news, not a reason to stop waiting for the child
        sigset_t sigwinch_bit = (sigset_t)1ULL << (SIGWINCH - 1);
        if (ready & ~(sigchld_bit | sigwinch_bit)) {
            current->wait_target_pid = 0;
            irq_restore(flags);
            return -EINTR;
        }

        current->state = TASK_BLOCKED;
        irq_restore(flags);

        sched_yield(get_current_task());
    }
}

long sys_waitpid(uint64_t pid) {
    int status;
    return wait_child(pid, &status);
}

long sys_waitstatus(uint64_t pid, int *user_status) {
    int status = 0;
    long r = wait_child(pid, &status);
    if (r < 0 || !user_status)
        return r;

    if (copy_to_user(get_current_task(), user_status, &status, sizeof(status)) != 0)
        return -EFAULT;
    return r;
}
