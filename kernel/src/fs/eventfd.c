#include <fs/eventfd.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <devices/type/tty_device.h>
#include <cpu/irq.h>
#include <ACPI/acpi_hpet.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <user/errno.h>
#include <string.h>

#define COUNT_MAX   0xFFFFFFFFFFFFFFFEULL

// shared bits

static int nonblock_ioctl(bool *nonblock, unsigned long request, void *arg) {
    task_t *task = get_current_task();
    if (!arg)
        return -EINVAL;
    switch (request) {
        case TTY_IOCTL_FIONBIO: {
            int on;
            if (copy_from_user(task, &on, arg, sizeof(on)) != 0) return -EFAULT;
            *nonblock = on != 0;
            return 0;
        }
        case TTY_IOCTL_SET_FLAGS: {
            uint32_t flags;
            if (copy_from_user(task, &flags, arg, sizeof(flags)) != 0) return -EFAULT;
            *nonblock = (flags & TTY_NONBLOCK) != 0;
            return 0;
        }
        case TTY_IOCTL_GET_FLAGS: {
            uint32_t flags = *nonblock ? TTY_NONBLOCK : 0;
            return copy_to_user(task, arg, &flags, sizeof(flags)) == 0 ? 0 : -EFAULT;
        }
        default:
            return -ENOTTY;
    }
}

static int install(const INodeOps_t *ops, void *data, int cloexec) {
    INode_t *inode = kmalloc(sizeof(INode_t));
    file_t  *f     = kmalloc(sizeof(file_t));
    if (!inode || !f) {
        kfree(inode);
        kfree(f);
        return -ENOMEM;
    }
    memset(inode, 0, sizeof(*inode));
    memset(f, 0, sizeof(*f));
    inode->type          = INODE_DEVICE;
    inode->ops           = ops;
    inode->internal_data = data;
    inode->shared        = 1;

    f->type   = FD_TYPE_DEV;
    f->inode  = inode;
    f->flags  = O_RDWR;
    f->shared = 1;

    int fd = vfs_install(f);
    if (fd < 0) {
        vfs_file_put(f);
        return fd;
    }
    if (cloexec)
        vfs_set_fd_flags(fd, FD_CLOEXEC);
    return fd;
}

// sleep on q till ready() says so, -EINTR on a signal or -EAGAIN at the deadline
static int block_on(wait_queue_t *q, bool (*ready)(void *), void *data, uint64_t deadline) {
    task_t  *current = get_current_task();
    sigset_t seen    = current->sig_pending;
    if (signal_should_interrupt(current))
        return -EINTR;
    unsigned long irq = irq_push();
    if (!ready(data) && current->sig_pending == seen)
        sched_wait_queue(q, deadline);
    irq_restore(irq);
    return 0;
}

// eventfd

typedef struct {
    uint64_t     count;
    bool         semaphore;
    bool         nonblock;
    wait_queue_t wait;
} evfd_t;

static bool ev_readable(void *p) {
    return ((evfd_t *)p)->count > 0;
}

static bool ev_writable(void *p) {
    return ((evfd_t *)p)->count < COUNT_MAX;
}

static long ev_read(INode_t *inode, void *out, size_t size, size_t offset) {
    (void)offset;
    evfd_t *e = inode->internal_data;
    if (size < sizeof(uint64_t))
        return -EINVAL;

    for (;;) {
        if (e->count) {
            uint64_t v = e->semaphore ? 1 : e->count;
            e->count -= v;
            memcpy(out, &v, sizeof(v));
            sched_wake_queue(&e->wait);
            return sizeof(v);
        }
        if (e->nonblock)
            return -EAGAIN;
        int err = block_on(&e->wait, ev_readable, e, 0);
        if (err)
            return err;
    }
}

static long ev_write(INode_t *inode, const void *in, size_t size, size_t offset) {
    (void)offset;
    evfd_t *e = inode->internal_data;
    uint64_t v;
    if (size < sizeof(v))
        return -EINVAL;
    memcpy(&v, in, sizeof(v));
    if (v == UINT64_MAX)
        return -EINVAL;

    for (;;) {
        if (COUNT_MAX - e->count >= v) {
            e->count += v;
            if (v)
                sched_wake_queue(&e->wait);
            return sizeof(v);
        }
        if (e->nonblock)
            return -EAGAIN;
        int err = block_on(&e->wait, ev_writable, e, 0);
        if (err)
            return err;
    }
}

static int ev_poll(INode_t *inode) {
    evfd_t *e = inode->internal_data;
    return (e->count ? POLLIN : 0) | (e->count < COUNT_MAX ? POLLOUT : 0);
}

static wait_queue_t *ev_queue(INode_t *inode) {
    return &((evfd_t *)inode->internal_data)->wait;
}

static int ev_ioctl(INode_t *inode, unsigned long request, void *arg) {
    return nonblock_ioctl(&((evfd_t *)inode->internal_data)->nonblock, request, arg);
}

static void ev_drop(INode_t *inode) {
    evfd_t *e = inode->internal_data;
    sched_wait_queue_destroy(&e->wait);
    kfree(e);
    inode->internal_data = NULL;
}

static const INodeOps_t eventfd_ops = {
    .read       = ev_read,
    .write      = ev_write,
    .poll       = ev_poll,
    .wait_queue = ev_queue,
    .ioctl      = ev_ioctl,
    .drop       = ev_drop,
};

int eventfd_create(uint64_t initval, int flags) {
    if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC))
        return -EINVAL;
    evfd_t *e = kmalloc(sizeof(evfd_t));
    if (!e)
        return -ENOMEM;
    memset(e, 0, sizeof(*e));
    e->count     = initval;
    e->semaphore = (flags & EFD_SEMAPHORE) != 0;
    e->nonblock  = (flags & EFD_NONBLOCK) != 0;

    int fd = install(&eventfd_ops, e, flags & EFD_CLOEXEC);
    if (fd < 0)
        kfree(e);
    return fd;
}

// timerfd, times are kept in hpet counts so they line up with sleep deadlines

typedef struct tfd {
    int          clock;
    bool         nonblock;
    bool         armed;
    uint64_t     expire;
    uint64_t     interval;  // counts, 0 for one shot
    uint64_t     ticks;     // expirations nobody has read yet
    wait_queue_t wait;
    struct tfd  *next;      // in the armed list the tick looks over
} tfd_t;

static tfd_t *armed_list;
static bool   tick_hooked;

static void tfd_unlink(tfd_t *t) {
    for (tfd_t **at = &armed_list; *at; at = &(*at)->next) {
        if (*at == t) {
            *at = t->next;
            break;
        }
    }
    t->next = NULL;
}

// moves expirations into ticks, true if any happened
static bool tfd_update(tfd_t *t) {
    if (!t->armed)
        return false;
    uint64_t now = hpet_read_counter();
    if (now < t->expire)
        return false;

    if (t->interval) {
        uint64_t n = (now - t->expire) / t->interval + 1;
        t->ticks  += n;
        t->expire += n * t->interval;
    } else {
        t->ticks++;
        t->armed = false;
        tfd_unlink(t);
    }
    return true;
}

// every tick, under the bkl like the syscalls that change the list
static void tfd_tick(void *ctx) {
    (void)ctx;
    tfd_t *t = armed_list;
    while (t) {
        tfd_t *next = t->next;
        if (tfd_update(t))
            sched_wake_queue(&t->wait);
        t = next;
    }
}

static bool tfd_ready(void *p) {
    tfd_t *t = p;
    tfd_update(t);
    return t->ticks > 0;
}

static long tfd_read(INode_t *inode, void *out, size_t size, size_t offset) {
    (void)offset;
    tfd_t *t = inode->internal_data;
    if (size < sizeof(uint64_t))
        return -EINVAL;

    for (;;) {
        unsigned long irq = irq_push();
        tfd_update(t);
        uint64_t got = t->ticks;
        t->ticks = 0;
        irq_restore(irq);

        if (got) {
            memcpy(out, &got, sizeof(got));
            return sizeof(got);
        }
        if (t->nonblock)
            return -EAGAIN;
        int err = block_on(&t->wait, tfd_ready, t, t->armed ? t->expire : 0);
        if (err)
            return err;
    }
}

static int tfd_poll(INode_t *inode) {
    tfd_t *t = inode->internal_data;
    tfd_update(t);
    return t->ticks ? POLLIN : 0;
}

static wait_queue_t *tfd_queue(INode_t *inode) {
    return &((tfd_t *)inode->internal_data)->wait;
}

static int tfd_ioctl(INode_t *inode, unsigned long request, void *arg) {
    return nonblock_ioctl(&((tfd_t *)inode->internal_data)->nonblock, request, arg);
}

static void tfd_drop(INode_t *inode) {
    tfd_t *t = inode->internal_data;
    unsigned long irq = irq_push();
    tfd_unlink(t);
    irq_restore(irq);
    sched_wait_queue_destroy(&t->wait);
    kfree(t);
    inode->internal_data = NULL;
}

static const INodeOps_t timerfd_ops = {
    .read       = tfd_read,
    .poll       = tfd_poll,
    .wait_queue = tfd_queue,
    .ioctl      = tfd_ioctl,
    .drop       = tfd_drop,
};

int timerfd_create(int clock, int flags) {
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != CLOCK_BOOTTIME)
        return -EINVAL;
    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC))
        return -EINVAL;

    if (!tick_hooked) {
        if (tick_register(tfd_tick, NULL) != 0)
            return -ENOMEM;
        tick_hooked = true;
    }

    tfd_t *t = kmalloc(sizeof(tfd_t));
    if (!t)
        return -ENOMEM;
    memset(t, 0, sizeof(*t));
    t->clock    = clock;
    t->nonblock = (flags & TFD_NONBLOCK) != 0;

    int fd = install(&timerfd_ops, t, flags & TFD_CLOEXEC);
    if (fd < 0)
        kfree(t);
    return fd;
}

static tfd_t *tfd_from_fd(int fd, file_t **out) {
    file_t *f = vfs_file_get(fd);
    if (!f)
        return NULL;
    if (!f->inode || f->inode->ops != &timerfd_ops) {
        vfs_file_put(f);
        return NULL;
    }
    *out = f;
    return f->inode->internal_data;
}

static bool ts_ok(const clock_timespec_t *ts) {
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

static uint64_t ts_ns(const clock_timespec_t *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static clock_timespec_t ns_ts(uint64_t ns) {
    return (clock_timespec_t){ (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL) };
}

static void tfd_current(tfd_t *t, itimerspec_t *cur) {
    uint64_t now  = hpet_read_counter();
    uint64_t left = t->armed && t->expire > now ? t->expire - now : 0;
    if (t->armed && !left)
        left = 1;
    cur->interval = ns_ts(clock_counts_to_ns(t->interval));
    cur->value    = ns_ts(t->armed ? clock_counts_to_ns(left) : 0);
}

int timerfd_settime(int fd, int flags, const itimerspec_t *value, itimerspec_t *old) {
    if (flags & ~TFD_TIMER_ABSTIME)
        return -EINVAL;
    if (!ts_ok(&value->value) || !ts_ok(&value->interval))
        return -EINVAL;

    file_t *f;
    tfd_t  *t = tfd_from_fd(fd, &f);
    if (!t)
        return -EINVAL;

    unsigned long irq = irq_push();
    if (old)
        tfd_current(t, old);

    tfd_unlink(t);
    t->armed    = false;
    t->ticks    = 0;
    t->interval = clock_ns_to_counts(ts_ns(&value->interval));

    uint64_t ns = ts_ns(&value->value);
    if (ns) {
        uint64_t now_counts = hpet_read_counter();
        uint64_t rel        = ns;
        if (flags & TFD_TIMER_ABSTIME) {
            uint64_t now;
            clock_read(t->clock, &now);
            rel = ns > now ? ns - now : 0;
        }
        t->expire = now_counts + clock_ns_to_counts(rel);
        t->armed  = true;
        t->next   = armed_list;
        armed_list = t;
    }
    irq_restore(irq);

    vfs_file_put(f);
    return 0;
}

int timerfd_gettime(int fd, itimerspec_t *cur) {
    file_t *f;
    tfd_t  *t = tfd_from_fd(fd, &f);
    if (!t)
        return -EINVAL;
    unsigned long irq = irq_push();
    tfd_update(t);
    tfd_current(t, cur);
    irq_restore(irq);
    vfs_file_put(f);
    return 0;
}
