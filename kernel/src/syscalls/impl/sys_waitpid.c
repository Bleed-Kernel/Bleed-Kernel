#include <stdint.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <user/signal.h>
#include <mm/spinlock.h>
#include <user/user_copy.h>

#define WNOHANG  1
#define WNOWAIT  0x1000000

// a zombie thats really finished
static int child_done(task_t *child) {
    return child->state == TASK_ZOMBIE && !task_group_alive(child);
}

// WNOHANG gives back 0 when nothing has finished, WNOWAIT leaves the zombie to be waited again
static long wait_child(int64_t pid, int *status, int options) {
    task_t *current = get_current_task();
    if (!current)
        return -ESRCH;
    int any = pid <= 0;

    for (;;) {
        // the check and the block have to be one step, a child exiting in between would
        // send its SIGCHLD before we are BLOCKED and nothing would ever wake us
        unsigned long flags = irq_push();

        task_t *found = NULL;
        int     kids  = 0;
        if (any) {
            task_t *leader = sched_get_task(current->tgid);
            for (task_t *c = leader ? leader->children : NULL; c; c = c->sibling_next) {
                // already waited for, the reaper just hasnt taken it off the list yet
                if (c->state == TASK_DEAD)
                    continue;
                kids++;
                if (child_done(c)) {
                    found = c;
                    break;
                }
            }
        } else {
            task_t *child = sched_get_task((uint64_t)pid);
            if (child && child->ppid == current->tgid && child->state != TASK_DEAD) {
                kids = 1;
                if (child_done(child))
                    found = child;
            }
        }

        if (!kids) {
            current->wait_target_pid = 0;
            irq_restore(flags);
            return -ECHILD;
        }

        if (found) {
            uint64_t child_pid = found->id;
            *status = found->exit_signal ? (found->exit_signal & 0x7F) : (found->exit_code & 0xFF) << 8;
            current->wait_target_pid = 0;
            irq_restore(flags);
            if (!(options & WNOWAIT))
                sched_mark_task_dead(found);
            return (long)child_pid;
        }

        if (options & WNOHANG) {
            current->wait_target_pid = 0;
            irq_restore(flags);
            return 0;
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

        current->wait_target_pid = any ? WAIT_ANY_CHILD : (uint64_t)pid;
        current->state = TASK_BLOCKED;
        irq_restore(flags);

        sched_yield(get_current_task());
    }
}

long sys_waitpid(uint64_t pid) {
    int status;
    if (pid == 0)
        return -EINVAL;
    return wait_child((int64_t)pid, &status, 0);
}

long sys_waitstatus(uint64_t pid, int *user_status) {
    int status = 0;
    if (pid == 0)
        return -EINVAL;
    long r = wait_child((int64_t)pid, &status, 0);
    if (r < 0 || !user_status)
        return r;

    if (copy_to_user(get_current_task(), user_status, &status, sizeof(status)) != 0)
        return -EFAULT;
    return r;
}

// waitpid with its options
long sys_waitopts(uint64_t pid, int *user_status, uint64_t options) {
    int status = 0;
    if (options & ~(uint64_t)(WNOHANG | WNOWAIT))
        return -EINVAL;
    long r = wait_child((int64_t)pid, &status, (int)options);
    if (r <= 0 || !user_status)
        return r;

    if (copy_to_user(get_current_task(), user_status, &status, sizeof(status)) != 0)
        return -EFAULT;
    return r;
}
