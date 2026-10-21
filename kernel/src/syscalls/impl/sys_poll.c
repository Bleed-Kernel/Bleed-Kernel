#include <fs/vfs.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <syscalls/syscall.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <mm/kalloc.h>

static short poll_one(task_t *task, const pollfd_t *p) {
    if (p->fd == POLL_FD_IPC)
        return (task->ipc_head && (p->events & POLLIN)) ? POLLIN : 0;

    // any other negative fd is skipped, thats how a caller mutes an entry
    if (p->fd < 0)
        return 0;
    if (p->fd >= MAX_FDS || !task->fd_table)
        return POLLNVAL;

    file_t *f = task->fd_table->fds[p->fd];
    if (!f || !f->inode)
        return POLLNVAL;

    // a regular file or a device with nothing to wait for never makes anyone sleep
    if (!f->inode->ops || !f->inode->ops->poll)
        return p->events & (POLLIN | POLLOUT);

    return (short)(f->inode->ops->poll(f->inode) & (p->events | POLLERR | POLLHUP));
}

// what to sleep on till this one might be ready, NULL if nothing ever changes it
static wait_queue_t *poll_queue(task_t *task, const pollfd_t *p) {
    if (p->fd == POLL_FD_IPC)
        return &task->ipc_wait;
    if (p->fd < 0 || p->fd >= MAX_FDS || !task->fd_table)
        return NULL;

    file_t *f = task->fd_table->fds[p->fd];
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->wait_queue)
        return NULL;
    return f->inode->ops->wait_queue(f->inode);
}

static long sys_poll_wait(task_t *current, pollfd_t *fds, wait_entry_t *waits,
                          uint64_t user_fds_ptr, uint64_t nfds, long timeout_ms);

/// @brief wait until one of fds is ready, a signal arrives or the timeout runs out
long sys_poll(uint64_t user_fds_ptr, uint64_t nfds, long timeout_ms) {
    task_t *current = get_current_task();
    if (!current)
        return -ESRCH;
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;
    if (nfds && (!user_fds_ptr || !user_ptr_valid(user_fds_ptr)))
        return -EFAULT;

    // most callers watch a handful, those stay on the stack
    pollfd_t      few[POLL_STACK_FDS];
    wait_entry_t  few_waits[POLL_STACK_FDS];
    pollfd_t     *fds   = few;
    wait_entry_t *waits = few_waits;
    if (nfds > POLL_STACK_FDS) {
        fds   = kmalloc((size_t)nfds * sizeof(pollfd_t));
        waits = kmalloc((size_t)nfds * sizeof(wait_entry_t));
        if (!fds || !waits) {
            kfree(fds);
            kfree(waits);
            return -ENOMEM;
        }
    }

    long ret = sys_poll_wait(current, fds, waits, user_fds_ptr, nfds, timeout_ms);
    if (fds != few) {
        kfree(fds);
        kfree(waits);
    }
    return ret;
}

static long sys_poll_wait(task_t *current, pollfd_t *fds, wait_entry_t *waits,
                          uint64_t user_fds_ptr, uint64_t nfds, long timeout_ms) {
    size_t bytes = (size_t)nfds * sizeof(pollfd_t);
    if (nfds && copy_from_user(current, fds, (void *)user_fds_ptr, bytes) != 0)
        return -EFAULT;

    int      timeout  = (int)timeout_ms;
    uint64_t deadline = timeout > 0 ? sched_deadline_ms((uint64_t)timeout) : 0;
    long     ready    = 0;

    for (;;) {
        // with no lock held and interrupts on, a fatal signal never comes back from this
        sigset_t seen = current->sig_pending;
        if (signal_should_interrupt(current))
            return -EINTR;

        unsigned long flags = irq_push();

        ready = 0;
        size_t queues = 0;
        for (size_t i = 0; i < nfds; i++) {
            fds[i].revents = poll_one(current, &fds[i]);
            if (fds[i].revents) ready++;

            wait_queue_t *queue = poll_queue(current, &fds[i]);
            if (queue)
                waits[queues++] = (wait_entry_t){ .queue = queue };
        }

        if (ready || timeout == 0 || (deadline && sched_deadline_passed(deadline))) {
            irq_restore(flags);
            break;
        }

        // a signal that slipped in since the last check
        if (current->sig_pending != seen) {
            irq_restore(flags);
            continue;
        }

        sched_wait_entries(waits, queues, deadline);
        irq_restore(flags);
    }

    if (nfds && copy_to_user(current, (void *)user_fds_ptr, fds, bytes) != 0)
        return -EFAULT;
    return ready;
}
