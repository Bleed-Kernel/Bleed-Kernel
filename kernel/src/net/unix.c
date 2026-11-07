/*
    I am so sorry, its a huge got a bit carried away, thanks to anyone who has ever 
    uploaded any resources for unix sockets theres a good chance i used it 
*/

#include <net/unix.h>
#include <net/socket.h>
#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <user/errno.h>
#include <devices/type/tty_device.h>
#include <string.h>

#define FIONREAD        0x541B
#define UNIX_CHUNK      16384   // stream sends get queued in pieces this big
#define UNIX_DGRAM_QLEN 256
#define UNIX_IOV_MAX    1024
#define UNIX_CTL_MAX    4096

// one send, a stream reads across them but never past one carrying fds
typedef struct unix_msg {
    struct unix_msg *next;
    size_t   len;
    size_t   off;
    int      nfiles;
    file_t **files;
    uint16_t from_len;
    char     from[UNIX_PATH_MAX];
    uint8_t  data[];
} unix_msg_t;

struct unix_sock {
    int          type;
    bool         nonblock;
    bool         listening;
    bool         connected;     // has or had a peer
    bool         rd_eof;        // the peer is gone or shut its writing side
    bool         wr_shut;
    int          owner_pid;
    int          peer_pid;
    unix_sock_t *peer;

    unix_msg_t  *rx_head;
    unix_msg_t  *rx_tail;
    size_t       rx_bytes;
    int          rx_count;

    // connections waiting on accept, they have no file till then
    unix_sock_t *pending_head;
    unix_sock_t *pending_tail;
    unix_sock_t *pending_next;
    int          pending;
    int          backlog;

    bool         bound;
    uint16_t     name_len;
    char         name[UNIX_PATH_MAX];
    uint16_t     peer_name_len;
    char         peer_name[UNIX_PATH_MAX];
    uint16_t     dest_len;      // a connected datagram socket sends here
    char         dest[UNIX_PATH_MAX];
    unix_sock_t *next_bound;

    uint64_t     rcvtimeo_ms;
    uint64_t     sndtimeo_ms;
    wait_queue_t wait;
};

static spinlock_t   unix_lock;
static unix_sock_t *bound_list;
static uint32_t     autobind_next;

static unsigned long unix_take(void) {
    unsigned long flags = irq_push();
    spinlock_acquire(&unix_lock);
    return flags;
}

static void unix_give(unsigned long flags) {
    spinlock_release(&unix_lock);
    irq_restore(flags);
}

static int current_pid(void) {
    task_t *t = get_current_task();
    return t ? (int)t->tgid : 0;
}

static int unix_poll_mask(unix_sock_t *u) {
    if (u->listening)
        return u->pending_head ? POLLIN : 0;

    int mask = 0;
    if (u->rx_head || u->rd_eof)
        mask |= POLLIN;
    if (u->type == SOCK_DGRAM)
        return mask | POLLOUT;
    if (!u->connected)
        return mask | POLLHUP;
    if (!u->peer)
        return mask | POLLIN | POLLHUP;
    if (!u->wr_shut && u->peer->rx_bytes < UNIX_RX_MAX)
        mask |= POLLOUT;
    return mask;
}

// sleep till want, 0 to look again, -EINTR or -EAGAIN when the deadline passes
static int unix_block(unix_sock_t *u, int want, uint64_t deadline) {
    task_t  *current = get_current_task();
    sigset_t seen    = current->sig_pending;
    if (signal_should_interrupt(current))
        return -EINTR;

    unsigned long irq = irq_push();
    if (!(unix_poll_mask(u) & (want | POLLERR | POLLHUP)) && current->sig_pending == seen)
        sched_wait_queue(&u->wait, deadline);
    irq_restore(irq);

    if (deadline && sched_deadline_passed(deadline))
        return -EAGAIN;
    return 0;
}

static void msg_free(unix_msg_t *m) {
    for (int i = 0; i < m->nfiles; i++)
        vfs_file_put(m->files[i]);
    kfree(m->files);
    kfree(m);
}

static void put_files(file_t **files, int n) {
    for (int i = 0; i < n; i++)
        vfs_file_put(files[i]);
    kfree(files);
}

// names, abstract ones start with a 0 and are compared as bytes

static unix_sock_t *find_bound(const char *name, uint16_t len) {
    for (unix_sock_t *s = bound_list; s; s = s->next_bound) {
        if (s->name_len == len && memcmp(s->name, name, len) == 0)
            return s;
    }
    return NULL;
}

static int name_from_user(const void *user_addr, uint64_t len, char *name, uint16_t *name_len) {
    struct {
        uint16_t family;
        char     path[UNIX_PATH_MAX];
    } sa;

    if (!user_addr || len < sizeof(uint16_t))
        return -EINVAL;
    memset(&sa, 0, sizeof(sa));
    if (copy_from_user(get_current_task(), &sa, user_addr, len < sizeof(sa) ? len : sizeof(sa)) != 0)
        return -EFAULT;
    if (sa.family != AF_UNIX)
        return -EAFNOSUPPORT;

    size_t n = len - sizeof(uint16_t);
    if (n > UNIX_PATH_MAX)
        n = UNIX_PATH_MAX;
    if (n && sa.path[0])
        n = strnlen(sa.path, n);

    memcpy(name, sa.path, n);
    *name_len = (uint16_t)n;
    return 0;
}

// writes what fits in room, *full gets the whole length
static int name_out(void *user_addr, uint32_t room, uint32_t *full, const char *name, uint16_t name_len) {
    struct {
        uint16_t family;
        char     path[UNIX_PATH_MAX];
    } sa;
    memset(&sa, 0, sizeof(sa));
    sa.family = AF_UNIX;
    memcpy(sa.path, name, name_len);

    // path names count their terminator like linux does
    *full = sizeof(uint16_t) + name_len + (name_len && name[0] && name_len < UNIX_PATH_MAX ? 1 : 0);
    uint32_t n = room < *full ? room : *full;
    if (n && copy_to_user(get_current_task(), user_addr, &sa, n) != 0)
        return -EFAULT;
    return 0;
}

static int name_to_user(void *user_addr, uint32_t *user_len, const char *name, uint16_t name_len) {
    if (!user_addr || !user_len)
        return 0;

    task_t  *task = get_current_task();
    uint32_t room, full;
    if (copy_from_user(task, &room, user_len, sizeof(room)) != 0)
        return -EFAULT;
    if (name_out(user_addr, room, &full, name, name_len) != 0 ||
        copy_to_user(task, user_len, &full, sizeof(full)) != 0)
        return -EFAULT;
    return 0;
}

// fd plumbing

static long unix_read_inode(INode_t *inode, void *out, size_t size, size_t offset);
static long unix_write_inode(INode_t *inode, const void *in, size_t size, size_t offset);
static int  unix_ioctl(INode_t *inode, unsigned long request, void *arg);
static int  unix_poll(INode_t *inode);
static wait_queue_t *unix_wait_queue(INode_t *inode);

static const INodeOps_t unix_ops = {
    .read       = unix_read_inode,
    .write      = unix_write_inode,
    .ioctl      = unix_ioctl,
    .poll       = unix_poll,
    .wait_queue = unix_wait_queue,
};

static unix_sock_t *unix_alloc(int type) {
    unix_sock_t *u = kmalloc(sizeof(unix_sock_t));
    if (!u)
        return NULL;
    memset(u, 0, sizeof(*u));
    u->type      = type;
    u->owner_pid = current_pid();
    return u;
}

static void unix_close(unix_sock_t *u);

// a file and fd around u, on failure u is gone too
static int unix_install(unix_sock_t *u) {
    INode_t *inode = kmalloc(sizeof(INode_t));
    file_t  *f     = kmalloc(sizeof(file_t));
    if (!inode || !f) {
        kfree(inode);
        kfree(f);
        unix_close(u);
        return -ENOMEM;
    }

    memset(inode, 0, sizeof(*inode));
    memset(f, 0, sizeof(*f));
    inode->type          = INODE_DEVICE;
    inode->ops           = &unix_ops;
    inode->internal_data = u;
    inode->shared        = 1;

    f->type   = FD_TYPE_UNIX;
    f->inode  = inode;
    f->flags  = O_RDWR;
    f->shared = 1;

    int fd = vfs_install(f);
    if (fd < 0)
        vfs_file_put(f);
    return fd;
}

static void unix_close(unix_sock_t *u) {
    unsigned long flags = unix_take();

    for (unix_sock_t **at = &bound_list; *at; at = &(*at)->next_bound) {
        if (*at == u) {
            *at = u->next_bound;
            break;
        }
    }

    if (u->peer) {
        unix_sock_t *p = u->peer;
        p->peer   = NULL;
        p->rd_eof = true;
        u->peer   = NULL;
        sched_wake_queue(&p->wait);
    }

    unix_msg_t  *rx      = u->rx_head;
    unix_sock_t *pending = u->pending_head;
    u->rx_head = u->rx_tail = NULL;
    u->pending_head = u->pending_tail = NULL;
    unix_give(flags);

    // these can be unix sockets too, so not with the lock held
    while (rx) {
        unix_msg_t *next = rx->next;
        msg_free(rx);
        rx = next;
    }
    while (pending) {
        unix_sock_t *next = pending->pending_next;
        unix_close(pending);
        pending = next;
    }

    sched_wait_queue_destroy(&u->wait);
    kfree(u);
}

void unix_file_release(file_t *f) {
    unix_sock_t *u = f->inode ? f->inode->internal_data : NULL;
    kfree(f->inode);
    if (u)
        unix_close(u);
}

unix_sock_t *unix_from_fd(int fd) {
    task_t *task = get_current_task();
    if (!task || !task->fd_table || fd < 0 || fd >= MAX_FDS)
        return NULL;
    file_t *f = task->fd_table->fds[fd];
    if (!f || f->type != FD_TYPE_UNIX || !f->inode)
        return NULL;
    return f->inode->internal_data;
}

static bool type_ok(int type) {
    return type == SOCK_STREAM || type == SOCK_DGRAM || type == SOCK_SEQPACKET;
}

int unix_socket_create(int type) {
    int kind = type & SOCK_TYPE_MASK;
    if (!type_ok(kind))
        return -EPROTONOSUPPORT;

    unix_sock_t *u = unix_alloc(kind);
    if (!u)
        return -ENOMEM;
    u->nonblock = (type & SOCK_NONBLOCK) != 0;
    return unix_install(u);
}

int unix_socketpair(int type, int fds[2]) {
    int kind = type & SOCK_TYPE_MASK;
    if (!type_ok(kind))
        return -EPROTONOSUPPORT;

    unix_sock_t *a = unix_alloc(kind);
    unix_sock_t *b = unix_alloc(kind);
    if (!a || !b) {
        kfree(a);
        kfree(b);
        return -ENOMEM;
    }

    a->peer = b;
    b->peer = a;
    a->connected = b->connected = true;
    a->peer_pid  = b->peer_pid  = current_pid();
    a->nonblock  = b->nonblock  = (type & SOCK_NONBLOCK) != 0;

    int fa = unix_install(a);
    if (fa < 0) {
        unix_close(b);
        return fa;
    }
    int fb = unix_install(b);
    if (fb < 0) {
        vfs_close(fa);
        return fb;
    }

    fds[0] = fa;
    fds[1] = fb;
    return 0;
}

int unix_bind(unix_sock_t *u, const void *user_addr, uint64_t len) {
    char     name[UNIX_PATH_MAX];
    uint16_t n;
    int err = name_from_user(user_addr, len, name, &n);
    if (err)
        return err;

    unsigned long flags = unix_take();
    if (u->bound) {
        unix_give(flags);
        return -EINVAL;
    }

    // nothing after the family asks for an abstract name of our choosing
    if (!n) {
        do {
            uint32_t id = ++autobind_next & 0xFFFFF;
            name[0] = 0;
            for (int i = 0; i < 5; i++)
                name[1 + i] = "0123456789abcdef"[(id >> (16 - i * 4)) & 0xF];
            n = 6;
        } while (find_bound(name, n));
    } else if (find_bound(name, n)) {
        unix_give(flags);
        return -EADDRINUSE;
    }

    memcpy(u->name, name, n);
    u->name_len   = n;
    u->bound      = true;
    u->next_bound = bound_list;
    bound_list    = u;
    unix_give(flags);
    return 0;
}

int unix_listen(unix_sock_t *u, int backlog) {
    if (u->type == SOCK_DGRAM)
        return -EOPNOTSUPP;
    if (!u->bound || u->connected)
        return -EINVAL;

    if (backlog < 1)
        backlog = 1;
    if (backlog > 128)
        backlog = 128;
    u->backlog   = backlog;
    u->listening = true;
    return 0;
}

int unix_connect(unix_sock_t *u, const void *user_addr, uint64_t len) {
    char     name[UNIX_PATH_MAX];
    uint16_t n;
    int err = name_from_user(user_addr, len, name, &n);
    if (err)
        return err;
    if (!n)
        return -EINVAL;

    // datagrams only remember where to send
    if (u->type == SOCK_DGRAM) {
        unsigned long flags = unix_take();
        unix_sock_t *t = find_bound(name, n);
        if (t && t->type == SOCK_DGRAM) {
            memcpy(u->dest, name, n);
            u->dest_len = n;
        }
        unix_give(flags);
        return t ? (t->type == SOCK_DGRAM ? 0 : -EPROTOTYPE) : (name[0] ? -ENOENT : -ECONNREFUSED);
    }

    if (u->listening)
        return -EINVAL;
    if (u->connected)
        return -EISCONN;

    unix_sock_t *s = unix_alloc(u->type);
    if (!s)
        return -ENOMEM;

    unsigned long flags = unix_take();
    unix_sock_t *l = find_bound(name, n);
    if (!l || !l->listening || l->type != u->type) {
        unix_give(flags);
        kfree(s);
        if (!l)
            return name[0] ? -ENOENT : -ECONNREFUSED;
        return l->type != u->type ? -EPROTOTYPE : -ECONNREFUSED;
    }
    // TODO: linux sleeps here when not nonblocking
    if (l->pending >= l->backlog) {
        unix_give(flags);
        kfree(s);
        return -EAGAIN;
    }

    s->peer      = u;
    u->peer      = s;
    s->connected = u->connected = true;
    s->owner_pid = l->owner_pid;
    s->peer_pid  = u->owner_pid;
    u->peer_pid  = l->owner_pid;

    // the accepted end is named like the listener, the connecting one sees that as its peer
    memcpy(s->name, l->name, l->name_len);
    s->name_len = l->name_len;
    memcpy(u->peer_name, l->name, l->name_len);
    u->peer_name_len = l->name_len;
    memcpy(s->peer_name, u->name, u->name_len);
    s->peer_name_len = u->name_len;

    if (l->pending_tail)
        l->pending_tail->pending_next = s;
    else
        l->pending_head = s;
    l->pending_tail = s;
    l->pending++;
    sched_wake_queue(&l->wait);
    unix_give(flags);
    return 0;
}

int unix_accept(unix_sock_t *u, int flags, void *user_addr, uint32_t *user_len) {
    if (!u->listening)
        return -EINVAL;

    uint64_t     deadline = u->rcvtimeo_ms ? sched_deadline_ms(u->rcvtimeo_ms) : 0;
    unix_sock_t *s;
    for (;;) {
        unsigned long irq = unix_take();
        s = u->pending_head;
        if (s) {
            u->pending_head = s->pending_next;
            if (!u->pending_head)
                u->pending_tail = NULL;
            s->pending_next = NULL;
            u->pending--;
        }
        unix_give(irq);
        if (s)
            break;

        if (u->nonblock)
            return -EAGAIN;
        int err = unix_block(u, POLLIN, deadline);
        if (err)
            return err;
    }

    s->nonblock = (flags & SOCK_NONBLOCK) != 0;
    char     peer[UNIX_PATH_MAX];
    uint16_t peer_len = s->peer_name_len;
    memcpy(peer, s->peer_name, peer_len);

    int fd = unix_install(s);
    if (fd < 0)
        return fd;

    int err = name_to_user(user_addr, user_len, peer, peer_len);
    return err ? err : fd;
}

int unix_shutdown(unix_sock_t *u, int how) {
    if (how < SHUT_RD || how > SHUT_RDWR)
        return -EINVAL;
    if (u->type != SOCK_DGRAM && !u->connected)
        return -ENOTCONN;

    unsigned long flags = unix_take();
    if (how != SHUT_WR) {
        u->rd_eof = true;
        sched_wake_queue(&u->wait);
    }
    if (how != SHUT_RD) {
        u->wr_shut = true;
        if (u->peer) {
            u->peer->rd_eof = true;
            sched_wake_queue(&u->peer->wait);
        }
        sched_wake_queue(&u->wait);
    }
    unix_give(flags);
    return 0;
}

int unix_name(unix_sock_t *u, bool peer, void *user_addr, uint32_t *user_len) {
    char     name[UNIX_PATH_MAX];
    uint16_t len;

    unsigned long flags = unix_take();
    if (peer && (!u->connected || !u->peer) && !u->dest_len) {
        unix_give(flags);
        return -ENOTCONN;
    }
    if (peer && u->type == SOCK_DGRAM && u->dest_len) {
        len = u->dest_len;
        memcpy(name, u->dest, len);
    } else {
        len = peer ? u->peer_name_len : u->name_len;
        memcpy(name, peer ? u->peer_name : u->name, len);
    }
    unix_give(flags);
    return name_to_user(user_addr, user_len, name, len);
}

int unix_setopt(unix_sock_t *u, int level, int name, const void *val, size_t len) {
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;

    switch (name) {
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            if (len < sizeof(sock_timeval_t))
                return -EINVAL;
            const sock_timeval_t *tv = val;
            if (tv->sec < 0 || tv->usec < 0 || tv->usec >= 1000000)
                return -EDOM;
            uint64_t ms = (uint64_t)tv->sec * 1000 + (uint64_t)tv->usec / 1000;
            if (!ms && tv->usec)
                ms = 1;
            if (name == SO_RCVTIMEO)
                u->rcvtimeo_ms = ms;
            else
                u->sndtimeo_ms = ms;
            return 0;
        }
        // nothing to tune, the sizes are fixed
        case SO_SNDBUF:
        case SO_RCVBUF:
        case SO_PASSCRED:
        case SO_REUSEADDR:
        case SO_KEEPALIVE:
            return 0;
        default:
            return -ENOPROTOOPT;
    }
}

int unix_getopt(unix_sock_t *u, int level, int name, void *val, size_t *len) {
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;

    int v;
    switch (name) {
        case SO_TYPE:       v = u->type; break;
        case SO_ERROR:      v = 0; break;
        case SO_ACCEPTCONN: v = u->listening; break;
        case SO_SNDBUF:
        case SO_RCVBUF:     v = UNIX_RX_MAX; break;
        case SO_PASSCRED:   v = 0; break;
        case SO_PEERCRED: {
            struct { int32_t pid; uint32_t uid; uint32_t gid; } cred = { u->peer_pid, 0, 0 };
            if (!u->connected)
                cred.pid = 0;
            size_t n = *len < sizeof(cred) ? *len : sizeof(cred);
            memcpy(val, &cred, n);
            *len = n;
            return 0;
        }
        default:
            return -ENOPROTOOPT;
    }

    size_t n = *len < sizeof(v) ? *len : sizeof(v);
    memcpy(val, &v, n);
    *len = n;
    return 0;
}

// walking an iovec list, user or kernel buffers

typedef struct {
    const user_iovec_t *v;
    int    count;
    int    i;
    size_t off;
    bool   user;
} iocur_t;

static int cur_in(iocur_t *c, void *dst, size_t len) {
    uint8_t *d = dst;
    while (len) {
        if (c->i >= c->count)
            return -EFAULT;
        size_t n = c->v[c->i].len - c->off;
        if (n > len)
            n = len;
        const void *src = (const void *)(uintptr_t)(c->v[c->i].base + c->off);
        if (c->user) {
            if (copy_from_user(get_current_task(), d, src, n) != 0)
                return -EFAULT;
        } else {
            memcpy(d, src, n);
        }
        d      += n;
        len    -= n;
        c->off += n;
        if (c->off == c->v[c->i].len) {
            c->i++;
            c->off = 0;
        }
    }
    return 0;
}

static int cur_out(iocur_t *c, const void *src, size_t len) {
    const uint8_t *s = src;
    while (len) {
        if (c->i >= c->count)
            return -EFAULT;
        size_t n = c->v[c->i].len - c->off;
        if (n > len)
            n = len;
        void *dst = (void *)(uintptr_t)(c->v[c->i].base + c->off);
        if (c->user) {
            if (copy_to_user(get_current_task(), dst, s, n) != 0)
                return -EFAULT;
        } else {
            memcpy(dst, s, n);
        }
        s      += n;
        len    -= n;
        c->off += n;
        if (c->off == c->v[c->i].len) {
            c->i++;
            c->off = 0;
        }
    }
    return 0;
}

static size_t iov_total(const user_iovec_t *v, int count) {
    size_t total = 0;
    for (int i = 0; i < count; i++)
        total += v[i].len;
    return total;
}

static unix_msg_t *msg_make(iocur_t *c, size_t len, unix_sock_t *from) {
    unix_msg_t *m = kmalloc(sizeof(unix_msg_t) + (len ? len : 1));
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->len = len;
    if (cur_in(c, m->data, len) != 0) {
        kfree(m);
        return NULL;
    }
    if (from->bound) {
        memcpy(m->from, from->name, from->name_len);
        m->from_len = from->name_len;
    }
    return m;
}

static void queue_msg(unix_sock_t *to, unix_msg_t *m) {
    if (to->rx_tail)
        to->rx_tail->next = m;
    else
        to->rx_head = m;
    to->rx_tail = m;
    to->rx_bytes += m->len;
    to->rx_count++;
    sched_wake_queue(&to->wait);
}

// files are ours to hand on or put back, on any return
static long send_stream(unix_sock_t *u, iocur_t *c, size_t total, int flags, file_t **files, int nfiles) {
    bool     packet   = u->type == SOCK_SEQPACKET;
    bool     nonblock = u->nonblock || (flags & MSG_DONTWAIT);
    uint64_t deadline = u->sndtimeo_ms ? sched_deadline_ms(u->sndtimeo_ms) : 0;
    size_t   sent     = 0;

    if (packet && total > UNIX_DGRAM_MAX) {
        put_files(files, nfiles);
        return -EMSGSIZE;
    }

    do {
        unsigned long irq = unix_take();
        long err = 0;
        if (!u->connected)
            err = -ENOTCONN;
        else if (u->wr_shut || !u->peer)
            err = -EPIPE;
        size_t room = 0;
        if (!err) {
            size_t queued = u->peer->rx_bytes;
            room = queued < UNIX_RX_MAX ? UNIX_RX_MAX - queued : 0;
            // a packet goes whole, an empty queue always takes one
            if (packet && room < total && queued)
                room = 0;
        }
        unix_give(irq);

        if (err) {
            put_files(files, nfiles);
            return sent ? (long)sent : err;
        }
        if (!room) {
            if (sent)
                break;
            if (nonblock) {
                put_files(files, nfiles);
                return -EAGAIN;
            }
            err = unix_block(u, POLLOUT, deadline);
            if (err) {
                put_files(files, nfiles);
                return err;
            }
            continue;
        }

        size_t n = packet ? total : total - sent;
        if (!packet) {
            if (n > room)
                n = room;
            if (n > UNIX_CHUNK)
                n = UNIX_CHUNK;
        }

        unix_msg_t *m = msg_make(c, n, u);
        if (!m) {
            put_files(files, nfiles);
            return sent ? (long)sent : -ENOMEM;
        }
        m->files  = files;
        m->nfiles = nfiles;
        files  = NULL;
        nfiles = 0;

        irq = unix_take();
        if (!u->peer) {
            unix_give(irq);
            msg_free(m);
            return sent ? (long)sent : -EPIPE;
        }
        queue_msg(u->peer, m);
        unix_give(irq);
        sent += n;
    } while (sent < total);

    put_files(files, nfiles);
    return (long)sent;
}

static long send_dgram(unix_sock_t *u, iocur_t *c, size_t total, int flags,
                       const char *to, uint16_t to_len, file_t **files, int nfiles) {
    if (total > UNIX_DGRAM_MAX) {
        put_files(files, nfiles);
        return -EMSGSIZE;
    }
    if (!to_len && !u->peer && !u->dest_len) {
        put_files(files, nfiles);
        return u->connected ? -ECONNREFUSED : -ENOTCONN;
    }

    unix_msg_t *m = msg_make(c, total, u);
    if (!m) {
        put_files(files, nfiles);
        return -ENOMEM;
    }
    m->files  = files;
    m->nfiles = nfiles;

    bool nonblock = u->nonblock || (flags & MSG_DONTWAIT);
    for (;;) {
        unsigned long irq = unix_take();
        unix_sock_t *t;
        if (to_len)
            t = find_bound(to, to_len);
        else if (u->peer)
            t = u->peer;
        else if (u->connected)
            t = NULL;
        else
            t = find_bound(u->dest, u->dest_len);

        if (!t || t->type != SOCK_DGRAM) {
            unix_give(irq);
            msg_free(m);
            return t ? -EPROTOTYPE : -ECONNREFUSED;
        }
        if (t->rx_count < UNIX_DGRAM_QLEN && (!t->rx_bytes || t->rx_bytes + total <= UNIX_RX_MAX)) {
            queue_msg(t, m);
            unix_give(irq);
            return (long)total;
        }
        unix_give(irq);

        if (nonblock) {
            msg_free(m);
            return -EAGAIN;
        }
        // the full one doesnt know about us, so look again every so often
        int err = unix_block(u, 0, sched_deadline_ms(10));
        if (err && err != -EAGAIN) {
            msg_free(m);
            return err;
        }
    }
}

static long unix_send(unix_sock_t *u, const user_iovec_t *v, int count, bool user, int flags,
                      const char *to, uint16_t to_len, file_t **files, int nfiles) {
    iocur_t c     = { .v = v, .count = count, .user = user };
    size_t  total = iov_total(v, count);

    if (u->listening) {
        put_files(files, nfiles);
        return -ENOTCONN;
    }
    if (u->type == SOCK_DGRAM)
        return send_dgram(u, &c, total, flags, to, to_len, files, nfiles);
    if (!total && !nfiles && u->type == SOCK_STREAM) {
        put_files(files, nfiles);
        return 0;
    }
    return send_stream(u, &c, total, flags, files, nfiles);
}

// what a recv took besides the bytes
typedef struct {
    file_t **files;
    int      nfiles;
    uint16_t from_len;
    char     from[UNIX_PATH_MAX];
    int      msg_flags;
} recv_extra_t;

static void take_files(recv_extra_t *x, unix_msg_t *m) {
    if (!m->nfiles)
        return;
    x->files  = m->files;
    x->nfiles = m->nfiles;
    m->files  = NULL;
    m->nfiles = 0;
}

static long unix_recv(unix_sock_t *u, const user_iovec_t *v, int count, bool user, int flags, recv_extra_t *x) {
    iocur_t  c        = { .v = v, .count = count, .user = user };
    size_t   total    = iov_total(v, count);
    bool     peek     = (flags & MSG_PEEK) != 0;
    bool     nonblock = u->nonblock || (flags & MSG_DONTWAIT);
    uint64_t deadline = u->rcvtimeo_ms ? sched_deadline_ms(u->rcvtimeo_ms) : 0;
    size_t   copied   = 0;

    if (u->listening)
        return -EINVAL;

    for (;;) {
        unsigned long irq = unix_take();
        if (!u->rx_head) {
            bool eof = u->rd_eof || (u->connected && !u->peer && u->type != SOCK_DGRAM);
            bool unconnected = u->type != SOCK_DGRAM && !u->connected;
            unix_give(irq);

            if (copied || eof)
                return (long)copied;
            if (unconnected)
                return -ENOTCONN;
            if (nonblock)
                return -EAGAIN;
            int err = unix_block(u, POLLIN, deadline);
            if (err)
                return err;
            continue;
        }

        // datagrams and packets come out one at a time, whats past len is lost
        if (u->type != SOCK_STREAM) {
            unix_msg_t *m = u->rx_head;
            size_t n = m->len < total ? m->len : total;
            if (cur_out(&c, m->data, n) != 0) {
                unix_give(irq);
                return -EFAULT;
            }
            if (m->len > total)
                x->msg_flags |= MSG_TRUNC;
            memcpy(x->from, m->from, m->from_len);
            x->from_len = m->from_len;
            size_t whole = m->len;

            if (!peek) {
                u->rx_head = m->next;
                if (!u->rx_head)
                    u->rx_tail = NULL;
                u->rx_bytes -= m->len;
                u->rx_count--;
                take_files(x, m);
                if (u->peer)
                    sched_wake_queue(&u->peer->wait);
            }
            unix_give(irq);
            if (!peek)
                msg_free(m);
            return (long)((flags & MSG_TRUNC) ? whole : n);
        }

        // a stream reads on across sends, but stops at one bringing fds after its taken any
        unix_msg_t *m   = u->rx_head;
        size_t      off = m->off;
        while (m && copied < total) {
            if (m->nfiles && (copied || x->nfiles))
                break;

            size_t n = m->len - off;
            if (n > total - copied)
                n = total - copied;
            if (cur_out(&c, m->data + off, n) != 0) {
                unix_give(irq);
                return copied ? (long)copied : -EFAULT;
            }
            copied += n;

            if (peek) {
                m   = m->next;
                off = m ? m->off : 0;
                continue;
            }

            m->off      += n;
            u->rx_bytes -= n;
            take_files(x, m);
            if (m->off < m->len)
                break;

            u->rx_head = m->next;
            if (!u->rx_head)
                u->rx_tail = NULL;
            u->rx_count--;
            unix_msg_t *done = m;
            m   = u->rx_head;
            off = m ? m->off : 0;
            kfree(done);
        }
        if (u->peer && !peek)
            sched_wake_queue(&u->peer->wait);
        unix_give(irq);

        if (!(flags & MSG_WAITALL) || peek || copied == total || x->nfiles)
            return (long)copied;
    }
}

// read and write, kernel buffers and no fds

static long unix_read_inode(INode_t *inode, void *out, size_t size, size_t offset) {
    (void)offset;
    user_iovec_t v = { (uint64_t)(uintptr_t)out, size };
    recv_extra_t x;
    memset(&x, 0, sizeof(x));
    long r = unix_recv(inode->internal_data, &v, 1, false, 0, &x);
    put_files(x.files, x.nfiles);
    return r;
}

static long unix_write_inode(INode_t *inode, const void *in, size_t size, size_t offset) {
    (void)offset;
    user_iovec_t v = { (uint64_t)(uintptr_t)in, size };
    return unix_send(inode->internal_data, &v, 1, false, 0, NULL, 0, NULL, 0);
}

static int unix_ioctl(INode_t *inode, unsigned long request, void *arg) {
    unix_sock_t *u    = inode->internal_data;
    task_t      *task = get_current_task();
    if (!arg)
        return -EINVAL;

    switch (request) {
        case TTY_IOCTL_FIONBIO: {
            int on;
            if (copy_from_user(task, &on, arg, sizeof(on)) != 0) return -EFAULT;
            u->nonblock = on != 0;
            return 0;
        }
        case TTY_IOCTL_SET_FLAGS: {
            uint32_t flags;
            if (copy_from_user(task, &flags, arg, sizeof(flags)) != 0) return -EFAULT;
            u->nonblock = (flags & TTY_NONBLOCK) != 0;
            return 0;
        }
        case TTY_IOCTL_GET_FLAGS: {
            uint32_t flags = u->nonblock ? TTY_NONBLOCK : 0;
            return copy_to_user(task, arg, &flags, sizeof(flags)) == 0 ? 0 : -EFAULT;
        }
        case FIONREAD: {
            unsigned long irq = unix_take();
            int n = 0;
            if (u->type == SOCK_STREAM)
                n = (int)u->rx_bytes;
            else if (u->rx_head)
                n = (int)u->rx_head->len;
            unix_give(irq);
            return copy_to_user(task, arg, &n, sizeof(n)) == 0 ? 0 : -EFAULT;
        }
        default:
            return -ENOTTY;
    }
}

static int unix_poll(INode_t *inode) {
    return unix_poll_mask(inode->internal_data);
}

static wait_queue_t *unix_wait_queue(INode_t *inode) {
    unix_sock_t *u = inode->internal_data;
    return &u->wait;
}

// sendmsg and recvmsg

static int copy_iov(const user_msghdr_t *msg, user_iovec_t **out) {
    *out = NULL;
    if (msg->iovlen > UNIX_IOV_MAX)
        return -EMSGSIZE;
    if (!msg->iovlen)
        return 0;

    user_iovec_t *v = kmalloc(msg->iovlen * sizeof(user_iovec_t));
    if (!v)
        return -ENOMEM;
    if (copy_from_user(get_current_task(), v, (const void *)msg->iov, msg->iovlen * sizeof(user_iovec_t)) != 0) {
        kfree(v);
        return -EFAULT;
    }

    size_t total = 0;
    for (uint64_t i = 0; i < msg->iovlen; i++) {
        if (__builtin_add_overflow(total, v[i].len, &total) || total > 0x7FFFFFFF) {
            kfree(v);
            return -EINVAL;
        }
    }
    *out = v;
    return 0;
}

// the fds in SCM_RIGHTS, each one held while its in flight
static int rights_from_user(const user_msghdr_t *msg, file_t ***out, int *count) {
    *out   = NULL;
    *count = 0;
    if (!msg->control || msg->controllen < sizeof(user_cmsghdr_t))
        return 0;
    if (msg->controllen > UNIX_CTL_MAX)
        return -ENOBUFS;

    uint8_t *ctl = kmalloc(msg->controllen);
    if (!ctl)
        return -ENOMEM;
    if (copy_from_user(get_current_task(), ctl, (const void *)msg->control, msg->controllen) != 0) {
        kfree(ctl);
        return -EFAULT;
    }

    file_t **files = NULL;
    int      n     = 0;
    int      err   = 0;
    size_t   off   = 0;
    while (off + sizeof(user_cmsghdr_t) <= msg->controllen) {
        user_cmsghdr_t *h = (user_cmsghdr_t *)(ctl + off);
        if (h->len < sizeof(user_cmsghdr_t) || h->len > msg->controllen - off) {
            err = -EINVAL;
            break;
        }

        if (h->level == SOL_SOCKET && h->type == SCM_RIGHTS) {
            int  more = (int)((h->len - sizeof(user_cmsghdr_t)) / sizeof(int));
            int *fds  = (int *)(h + 1);
            if (n + more > UNIX_FDS_MAX) {
                err = -ETOOMANYREFS;
                break;
            }
            if (!files && !(files = kmalloc(UNIX_FDS_MAX * sizeof(file_t *)))) {
                err = -ENOMEM;
                break;
            }
            for (int i = 0; i < more; i++) {
                file_t *f = vfs_file_get(fds[i]);
                if (!f) {
                    err = -EBADF;
                    break;
                }
                files[n++] = f;
            }
            if (err)
                break;
        }
        off += (h->len + 7) & ~(uint64_t)7;
    }
    kfree(ctl);

    if (err) {
        put_files(files, n);
        return err;
    }
    if (!n) {
        kfree(files);
        files = NULL;
    }
    *out   = files;
    *count = n;
    return 0;
}

long unix_sendmsg(unix_sock_t *u, const user_msghdr_t *msg, int flags) {
    char     to[UNIX_PATH_MAX];
    uint16_t to_len = 0;
    if (msg->name && msg->namelen && u->type == SOCK_DGRAM) {
        int err = name_from_user((const void *)msg->name, msg->namelen, to, &to_len);
        if (err)
            return err;
    }

    user_iovec_t *v;
    int err = copy_iov(msg, &v);
    if (err)
        return err;

    file_t **files;
    int      nfiles;
    err = rights_from_user(msg, &files, &nfiles);
    if (err) {
        kfree(v);
        return err;
    }

    long r = unix_send(u, v, (int)msg->iovlen, true, flags, to, to_len, files, nfiles);
    kfree(v);
    return r;
}

// fds go into the callers table now, the ones without room in control get closed
static int rights_to_user(user_msghdr_t *msg, recv_extra_t *x, bool cloexec) {
    task_t  *task  = get_current_task();
    uint64_t room  = msg->control && msg->controllen > sizeof(user_cmsghdr_t)
                   ? (msg->controllen - sizeof(user_cmsghdr_t)) / sizeof(int) : 0;
    int      fds[UNIX_FDS_MAX];
    int      given = 0;

    for (int i = 0; i < x->nfiles; i++) {
        int fd = -1;
        if ((uint64_t)given < room)
            fd = vfs_install(x->files[i]);
        if (fd < 0) {
            vfs_file_put(x->files[i]);
            msg->flags |= MSG_CTRUNC;
            continue;
        }
        if (cloexec)
            vfs_set_fd_flags(fd, FD_CLOEXEC);
        fds[given++] = fd;
    }
    kfree(x->files);
    x->files  = NULL;
    x->nfiles = 0;

    if (!given) {
        msg->controllen = 0;
        return 0;
    }

    user_cmsghdr_t h = {
        .len   = sizeof(user_cmsghdr_t) + given * sizeof(int),
        .level = SOL_SOCKET,
        .type  = SCM_RIGHTS,
    };
    if (copy_to_user(task, (void *)msg->control, &h, sizeof(h)) != 0 ||
        copy_to_user(task, (void *)(msg->control + sizeof(h)), fds, given * sizeof(int)) != 0)
        return -EFAULT;
    msg->controllen = (h.len + 7) & ~(uint64_t)7;
    return 0;
}

long unix_recvmsg(unix_sock_t *u, user_msghdr_t *msg, int flags) {
    user_iovec_t *v;
    int err = copy_iov(msg, &v);
    if (err)
        return err;

    recv_extra_t x;
    memset(&x, 0, sizeof(x));
    long r = unix_recv(u, v, (int)msg->iovlen, true, flags, &x);
    kfree(v);

    msg->flags = x.msg_flags;
    err = rights_to_user(msg, &x, (flags & MSG_CMSG_CLOEXEC) != 0);
    if (r < 0)
        return r;
    if (err)
        return err;

    if (msg->name) {
        uint32_t full;
        err = name_out((void *)msg->name, msg->namelen, &full, x.from, x.from_len);
        msg->namelen = full;
    }
    return err ? err : r;
}

long unix_sendto(unix_sock_t *u, const void *user_buf, size_t len, int flags, const void *user_addr, uint64_t addr_len) {
    char     to[UNIX_PATH_MAX];
    uint16_t to_len = 0;
    if (user_addr && u->type == SOCK_DGRAM) {
        int err = name_from_user(user_addr, addr_len, to, &to_len);
        if (err)
            return err;
    }
    user_iovec_t v = { (uint64_t)(uintptr_t)user_buf, len };
    return unix_send(u, &v, 1, true, flags, to, to_len, NULL, 0);
}

long unix_recvfrom(unix_sock_t *u, void *user_buf, size_t len, int flags, void *user_addr, uint32_t *user_len) {
    user_iovec_t v = { (uint64_t)(uintptr_t)user_buf, len };
    recv_extra_t x;
    memset(&x, 0, sizeof(x));
    long r = unix_recv(u, &v, 1, true, flags, &x);
    put_files(x.files, x.nfiles);
    if (r < 0)
        return r;
    int err = name_to_user(user_addr, user_len, x.from, x.from_len);
    return err ? err : r;
}
