#include <fs/epoll.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <user/signal.h>
#include <user/errno.h>
#include <string.h>

// an interest is keyed by fd and the file behind it, it holds no ref
typedef struct {
    int      fd;
    file_t  *file;
    uint32_t events;
    uint64_t data;
    uint32_t out_seen;  // EPOLLET, POLLOUT was reported and hasnt gone away since
    bool     disarmed;  // EPOLLONESHOT fired, needs a MOD
} epoll_item_t;

typedef struct {
    epoll_item_t *items;
    int           count;
    int           cap;
} epoll_t;

static int epoll_poll(INode_t *inode);

static const INodeOps_t epoll_ops = {
    .poll = epoll_poll,
};

static epoll_t *epoll_from_fd(int fd) {
    task_t *task = get_current_task();
    if (!task || !task->fd_table || fd < 0 || fd >= MAX_FDS)
        return NULL;
    file_t *f = task->fd_table->fds[fd];
    if (!f || f->type != FD_TYPE_EPOLL || !f->inode)
        return NULL;
    return f->inode->internal_data;
}

static bool item_alive(task_t *task, const epoll_item_t *it) {
    return task->fd_table && it->fd >= 0 && it->fd < MAX_FDS && task->fd_table->fds[it->fd] == it->file;
}

static void item_remove(epoll_t *ep, int i) {
    ep->items[i] = ep->items[--ep->count];
}

static uint32_t item_ready(const epoll_item_t *it) {
    const INodeOps_t *ops = it->file->inode ? it->file->inode->ops : NULL;
    uint32_t mask = (ops && ops->poll) ? (uint32_t)ops->poll(it->file->inode) : (POLLIN | POLLOUT);

    if ((mask & POLLHUP) && (it->events & EPOLLRDHUP))
        mask |= EPOLLRDHUP;
    return mask & (it->events | POLLERR | POLLHUP);
}

int epoll_create(int flags) {
    if (flags & ~EPOLL_CLOEXEC)
        return -EINVAL;

    epoll_t *ep    = kmalloc(sizeof(epoll_t));
    INode_t *inode = kmalloc(sizeof(INode_t));
    file_t  *f     = kmalloc(sizeof(file_t));
    if (!ep || !inode || !f) {
        kfree(ep);
        kfree(inode);
        kfree(f);
        return -ENOMEM;
    }
    memset(ep, 0, sizeof(*ep));
    memset(inode, 0, sizeof(*inode));
    memset(f, 0, sizeof(*f));

    inode->type          = INODE_DEVICE;
    inode->ops           = &epoll_ops;
    inode->internal_data = ep;
    inode->shared        = 1;

    f->type   = FD_TYPE_EPOLL;
    f->inode  = inode;
    f->flags  = O_RDONLY;
    f->shared = 1;

    int fd = vfs_install(f);
    if (fd < 0) {
        vfs_file_put(f);
        return fd;
    }
    if (flags & EPOLL_CLOEXEC)
        vfs_set_fd_flags(fd, FD_CLOEXEC);
    return fd;
}

void epoll_file_release(file_t *f) {
    epoll_t *ep = f->inode ? f->inode->internal_data : NULL;
    if (ep) {
        kfree(ep->items);
        kfree(ep);
    }
    kfree(f->inode);
}

int epoll_ctl(int epfd, int op, int fd, const epoll_event_t *ev) {
    task_t  *task = get_current_task();
    epoll_t *ep   = epoll_from_fd(epfd);
    if (!ep)
        return task && task->fd_table && epfd >= 0 && epfd < MAX_FDS && task->fd_table->fds[epfd] ? -EINVAL : -EBADF;
    if (fd < 0 || fd >= MAX_FDS || !task->fd_table->fds[fd])
        return -EBADF;
    if (fd == epfd)
        return -EINVAL;

    file_t *f = task->fd_table->fds[fd];
    // regular files are always ready, linux turns them away
    if (f->type == FD_TYPE_FS && f->inode && f->inode->type != INODE_DEVICE)
        return -EPERM;

    // drop whatever went stale so a reused fd number isnt EEXIST
    int at = -1;
    for (int i = 0; i < ep->count; ) {
        if (!item_alive(task, &ep->items[i])) {
            item_remove(ep, i);
            continue;
        }
        if (ep->items[i].fd == fd)
            at = i;
        i++;
    }

    switch (op) {
        case EPOLL_CTL_ADD: {
            if (at >= 0)
                return -EEXIST;
            if (ep->count == ep->cap) {
                int cap = ep->cap ? ep->cap * 2 : 16;
                epoll_item_t *more = kmalloc((size_t)cap * sizeof(epoll_item_t));
                if (!more)
                    return -ENOMEM;
                if (ep->count)
                    memcpy(more, ep->items, (size_t)ep->count * sizeof(epoll_item_t));
                kfree(ep->items);
                ep->items = more;
                ep->cap   = cap;
            }
            ep->items[ep->count++] = (epoll_item_t){
                .fd = fd, .file = f, .events = ev->events, .data = ev->data,
            };
            return 0;
        }
        case EPOLL_CTL_MOD:
            if (at < 0)
                return -ENOENT;
            ep->items[at].events   = ev->events;
            ep->items[at].data     = ev->data;
            ep->items[at].out_seen = 0;
            ep->items[at].disarmed = false;
            return 0;
        case EPOLL_CTL_DEL:
            if (at < 0)
                return -ENOENT;
            item_remove(ep, at);
            return 0;
        default:
            return -EINVAL;
    }
}

// what goes out this round, out has room for max
static int epoll_collect(task_t *task, epoll_t *ep, epoll_event_t *out, int max, wait_entry_t *waits, size_t *queues) {
    int n = 0;
    *queues = 0;

    for (int i = 0; i < ep->count; ) {
        epoll_item_t *it = &ep->items[i];
        if (!item_alive(task, it)) {
            item_remove(ep, i);
            continue;
        }
        i++;
        if (it->disarmed)
            continue;

        const INodeOps_t *ops = it->file->inode ? it->file->inode->ops : NULL;
        if (waits && ops && ops->wait_queue) {
            wait_queue_t *q = ops->wait_queue(it->file->inode);
            if (q)
                waits[(*queues)++] = (wait_entry_t){ .queue = q };
        }

        uint32_t mask = item_ready(it);

        // edge triggered only holds back POLLOUT, readable stays level so a reader that
        if (it->events & EPOLLET) {
            if (!(mask & POLLOUT))
                it->out_seen = 0;
            else if (it->out_seen)
                mask &= ~(uint32_t)POLLOUT;
        }
        if (!mask || n >= max)
            continue;

        if (mask & POLLOUT)
            it->out_seen = 1;
        if (it->events & EPOLLONESHOT)
            it->disarmed = true;
        out[n].events = mask;
        out[n].data   = it->data;
        n++;
    }
    return n;
}

static int epoll_poll(INode_t *inode) {
    epoll_t *ep   = inode->internal_data;
    task_t  *task = get_current_task();
    for (int i = 0; i < ep->count; i++) {
        epoll_item_t *it = &ep->items[i];
        if (item_alive(task, it) && !it->disarmed && item_ready(it))
            return POLLIN;
    }
    return 0;
}

long epoll_wait(int epfd, void *user_events, int max, long timeout_ms, const sigset_t *mask) {
    task_t  *current = get_current_task();
    epoll_t *ep      = epoll_from_fd(epfd);
    if (!ep)
        return -EBADF;
    if (max <= 0 || max > 4096)
        return -EINVAL;
    if (!user_events || !user_ptr_valid((uint64_t)user_events))
        return -EFAULT;

    epoll_event_t *out = kmalloc((size_t)max * sizeof(epoll_event_t));
    if (!out)
        return -ENOMEM;

    sigset_t saved;
    if (mask)
        signal_set_mask(current, SIG_SETMASK, mask, &saved);

    uint64_t deadline = timeout_ms > 0 ? sched_deadline_ms((uint64_t)timeout_ms) : 0;
    long     ret      = 0;
    for (;;) {
        sigset_t seen = current->sig_pending;
        if (signal_should_interrupt(current)) {
            ret = -EINTR;
            break;
        }

        // sized every round, a ctl from another thread can grow the list while we sleep
        wait_entry_t *waits  = ep->count ? kmalloc((size_t)ep->count * sizeof(wait_entry_t)) : NULL;
        size_t        queues = 0;

        unsigned long flags = irq_push();
        ret = epoll_collect(current, ep, out, max, waits, &queues);
        if (ret || timeout_ms == 0 || (deadline && sched_deadline_passed(deadline))) {
            irq_restore(flags);
            kfree(waits);
            break;
        }
        if (current->sig_pending == seen)
            sched_wait_entries(waits, queues, deadline);
        irq_restore(flags);
        kfree(waits);
    }

    if (mask)
        signal_set_mask(current, SIG_SETMASK, &saved, NULL);

    if (ret > 0 && copy_to_user(current, user_events, out, (size_t)ret * sizeof(epoll_event_t)) != 0)
        ret = -EFAULT;
    kfree(out);
    return ret;
}
