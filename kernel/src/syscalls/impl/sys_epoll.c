#include <syscalls/syscall.h>
#include <ipc/epoll.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>

static epoll_instance_t *resolve_epoll_fd(task_t *task, int epfd) {
    if (epfd < 0 || epfd >= MAX_FDS || !task->fd_table)
        return NULL;

    file_t *f = task->fd_table->fds[epfd];
    if (!f || f->type != FD_TYPE_EPOLL)
        return NULL;

    return (epoll_instance_t *)f->inode->internal_data;
}

static poll_table_t *resolve_poll_table(task_t *task, int fd) {
    if (fd < 0 || fd >= MAX_FDS || !task->fd_table)
        return NULL;

    file_t *f = task->fd_table->fds[fd];
    if (!f)
        return NULL;

    if (f->type == FD_TYPE_EPOLL)
        return NULL;

    // only ipc fd supported for now — poll table lives on the task
    return &task->ipc_poll;
}

static int alloc_fd(fd_table_t *table) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!table->fds[i])
            return i;
    }
    return -EMFILE;
}

long sys_epoll_create1(int flags) {
    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;

    task_t *task = get_current_task();
    if (!task || !task->fd_table)
        return -ESRCH;

    epoll_instance_t *ep = epoll_instance_create();
    if (!ep)
        return -ENOMEM;

    // epoll fd uses a synthetic inode to hold the instance pointer
    INode_t *inode = kmalloc(sizeof(INode_t));
    if (!inode) {
        epoll_instance_put(ep);
        return -ENOMEM;
    }
    inode->shared        = 1;
    inode->type          = INODE_FILE;
    inode->ops           = NULL;
    inode->internal_data = ep;
    inode->parent        = NULL;
    inode->name[0]       = '\0';

    file_t *f = kmalloc(sizeof(file_t));
    if (!f) {
        kfree(inode, sizeof(INode_t));
        epoll_instance_put(ep);
        return -ENOMEM;
    }
    f->type   = FD_TYPE_EPOLL;
    f->inode  = inode;
    f->offset = 0;
    f->flags  = flags;
    f->shared = 1;

    int fd = alloc_fd(task->fd_table);
    if (fd < 0) {
        kfree(f, sizeof(file_t));
        kfree(inode, sizeof(INode_t));
        epoll_instance_put(ep);
        return fd;
    }

    task->fd_table->fds[fd] = f;
    return (long)fd;
}

long sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *user_ev) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    if (epfd == fd)
        return -EINVAL;

    epoll_instance_t *ep = resolve_epoll_fd(task, epfd);
    if (!ep)
        return -EBADF;

    epoll_event_t ev = {0};
    if (op != EPOLL_CTL_DEL) {
        if (!user_ev || !user_ptr_valid((uint64_t)(uintptr_t)user_ev))
            return -EFAULT;
        if (copy_from_user(task, &ev, user_ev, sizeof(epoll_event_t)) != 0)
            return -EFAULT;
    }

    switch (op) {
        case EPOLL_CTL_ADD: {
            poll_table_t *pt = resolve_poll_table(task, fd);
            if (!pt)
                return -EBADF;
            return (long)epoll_ctl_add(ep, fd, &ev, pt);
        }
        case EPOLL_CTL_MOD:
            return (long)epoll_ctl_mod(ep, fd, &ev);
        case EPOLL_CTL_DEL: {
            poll_table_t *pt = resolve_poll_table(task, fd);
            return (long)epoll_ctl_del(ep, fd, pt);
        }
        default:
            return -EINVAL;
    }
}

long sys_epoll_wait(int epfd, epoll_event_t *user_events, int maxevents, int timeout_ms) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    if (maxevents <= 0)
        return -EINVAL;

    if (!user_events || !user_ptr_valid((uint64_t)(uintptr_t)user_events))
        return -EFAULT;

    epoll_instance_t *ep = resolve_epoll_fd(task, epfd);
    if (!ep)
        return -EBADF;

    epoll_event_t *kbuf = kmalloc((size_t)maxevents * sizeof(epoll_event_t));
    if (!kbuf)
        return -ENOMEM;

    int n = epoll_wait_events(ep, task, kbuf, maxevents, timeout_ms);
    if (n < 0) {
        kfree(kbuf, (size_t)maxevents * sizeof(epoll_event_t));
        return (long)n;
    }

    if (n > 0) {
        if (copy_to_user(task, user_events, kbuf, (size_t)n * sizeof(epoll_event_t)) != 0) {
            kfree(kbuf, (size_t)maxevents * sizeof(epoll_event_t));
            return -EFAULT;
        }
    }

    kfree(kbuf, (size_t)maxevents * sizeof(epoll_event_t));
    return (long)n;
}

long sys_epoll_pwait(int epfd, epoll_event_t *user_events, int maxevents, int timeout_ms,
                     const uint64_t *user_sigmask, size_t sigsetsize) {
    (void)user_sigmask;
    (void)sigsetsize;
    return sys_epoll_wait(epfd, user_events, maxevents, timeout_ms);
}

// call from sys_close before the fd slot is cleared
void epoll_fd_close(task_t *task, int fd) {
    if (!task || !task->fd_table || fd < 0 || fd >= MAX_FDS)
        return;

    file_t *f = task->fd_table->fds[fd];
    if (!f)
        return;

    if (f->type == FD_TYPE_EPOLL) {
        epoll_instance_put((epoll_instance_t *)f->inode->internal_data);
        kfree(f->inode, sizeof(INode_t));
        kfree(f, sizeof(file_t));
        task->fd_table->fds[fd] = NULL;
        return;
    }

    // non-epoll fd closing: remove it from every epoll watching it
    poll_table_t *pt = resolve_poll_table(task, fd);
    if (!pt)
        return;

    for (int i = 0; i < MAX_FDS; i++) {
        file_t *e = task->fd_table->fds[i];
        if (!e || e->type != FD_TYPE_EPOLL)
            continue;
        epoll_ctl_del((epoll_instance_t *)e->inode->internal_data, fd, pt);
    }
}