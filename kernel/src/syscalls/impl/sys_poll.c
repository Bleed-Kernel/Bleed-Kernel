#include <fs/vfs.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <syscalls/syscall.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <user/user_poll.h>

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

/// @brief wait until one of fds is ready, a signal arrives or the timeout runs out
long sys_poll(uint64_t user_fds_ptr, uint64_t nfds, long timeout_ms) {
    task_t *current = get_current_task();
    if (!current)
        return -ESRCH;
    if (nfds > POLL_MAX_FDS)
        return -EINVAL;
    if (nfds && (!user_fds_ptr || !user_ptr_valid(user_fds_ptr)))
        return -EFAULT;

    pollfd_t fds[POLL_MAX_FDS];
    size_t   bytes = (size_t)nfds * sizeof(pollfd_t);
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
        for (size_t i = 0; i < nfds; i++) {
            fds[i].revents = poll_one(current, &fds[i]);
            if (fds[i].revents) ready++;
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

        sched_wait_event(deadline);
        irq_restore(flags);
    }

    if (nfds && copy_to_user(current, (void *)user_fds_ptr, fds, bytes) != 0)
        return -EFAULT;
    return ready;
}
