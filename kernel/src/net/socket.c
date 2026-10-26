#include <net/socket.h>
#include <net/udp.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <fs/vfs.h>
#include <drivers/serial/serial.h>
#include <devices/type/tty_device.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <user/user_copy.h>
#include <user/user_poll.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

#define FIONREAD    0x541B
#define TCP_NODELAY 1
#define SOCK_CHUNK  16384   // stream bounce buffer

// a queued udp datagram
typedef struct sock_dgram {
    struct sock_dgram *next;
    uint32_t           src;
    uint16_t           port;
    uint16_t           len;
    uint8_t            data[];
} sock_dgram_t;

struct socket {
    struct socket *next;        // in the bound list
    int            type;        // SOCK_DGRAM or SOCK_STREAM
    tcb_t         *tcb;         // the connection, stream sockets only
    uint32_t       local_ip;    // 0 is any?
    uint16_t       local_port;  // host order, 0 till its bound
    uint32_t       remote_ip;
    uint16_t       remote_port;
    bool           connected;
    bool           nonblock;
    bool           broadcast;
    bool           reuse;
    uint64_t       rcvtimeo_ms;
    sock_dgram_t  *rx_head;
    sock_dgram_t  *rx_tail;
    int            rx_count;
    uint64_t       rx_dropped;
    wait_queue_t   wait;
};

static socket_t  *bound;
static spinlock_t sock_lock;
static uint16_t   next_port = SOCK_PORT_FIRST;

static bool is_loopback(uint32_t ip) {
    return (ip & 0xFF) == 127;
}

// kernel buffers from read/write, user ones from the socket syscalls
static int sock_copy_out(void *dst, const void *src, size_t len, bool user) {
    if (!user) {
        memcpy(dst, src, len);
        return 0;
    }
    return copy_to_user(get_current_task(), dst, src, len) == 0 ? 0 : -EFAULT;
}

static int sock_poll_mask(socket_t *s) {
    if (s->type == SOCK_STREAM)
        return s->tcb ? tcp_poll(s->tcb) : POLLERR;
    return (s->rx_head ? POLLIN : 0) | POLLOUT;
}

// sleep till want is ready, 0 to look again, -EINTR or -EAGAIN on timeout
static int sock_block(socket_t *s, int want, uint64_t deadline) {
    task_t  *current = get_current_task();
    sigset_t seen    = current->sig_pending;
    if (signal_should_interrupt(current))
        return -EINTR;

    unsigned long irq = irq_push();
    if (!(sock_poll_mask(s) & (want | POLLERR | POLLHUP)) && current->sig_pending == seen)
        sched_wait_queue(&s->wait, deadline);
    irq_restore(irq);

    if (deadline && sched_deadline_passed(deadline))
        return -EAGAIN;
    return 0;
}

static bool port_taken_locked(uint16_t port) {
    for (socket_t *s = bound; s; s = s->next) {
        if (s->local_port == port)
            return true;
    }
    return udp_port_listening(port);
}

bool socket_port_used(uint16_t port) {
    unsigned long flags = irq_push();
    spinlock_acquire(&sock_lock);
    bool used = port_taken_locked(port);
    spinlock_release(&sock_lock);
    irq_restore(flags);
    return used;
}

static int bind_locked(socket_t *s, uint32_t ip, uint16_t port) {
    if (s->local_port)
        return -EINVAL;

    if (!port) {
        int range = SOCK_PORT_LAST - SOCK_PORT_FIRST + 1;
        for (int i = 0; i < range && !port; i++) {
            uint16_t p = next_port;
            next_port  = next_port == SOCK_PORT_LAST ? SOCK_PORT_FIRST : next_port + 1;
            if (!port_taken_locked(p))
                port = p;
        }
        if (!port)
            return -EADDRINUSE;
    } else if (port_taken_locked(port)) {
        return -EADDRINUSE;
    }

    s->local_ip   = ip;
    s->local_port = port;
    s->next       = bound;
    bound         = s;
    return 0;
}

int socket_bind(socket_t *s, uint32_t ip, uint16_t port) {
    netif_t *nif = netif_default();
    if (ip && !is_loopback(ip) && (!nif || ip != nif->ip))
        return -EADDRNOTAVAIL;

    unsigned long flags = irq_push();
    spinlock_acquire(&sock_lock);
    int err = bind_locked(s, ip, port);
    spinlock_release(&sock_lock);
    irq_restore(flags);

    if (!err)
        serial_printf(LOG_INFO "socket: bound to " IP_FMT ":%u\n", IP_ARGS(ip), s->local_port);
    return err;
}

// anything sending or connecting before a bind gets a port of its own
static int autobind(socket_t *s) {
    if (s->local_port)
        return 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&sock_lock);
    int err = s->local_port ? 0 : bind_locked(s, 0, 0);
    spinlock_release(&sock_lock);
    irq_restore(flags);
    return err;
}

int socket_connect(socket_t *s, uint32_t ip, uint16_t port) {
    if (!port)
        return -EINVAL;
    int err = autobind(s);
    if (err)
        return err;

    if (!ip)
        ip = IP4(127, 0, 0, 1);
    if (s->type == SOCK_DGRAM) {
        s->remote_ip   = ip;
        s->remote_port = port;
        s->connected   = true;
        return 0;
    }

    err = tcp_connect(s->tcb, s->local_ip, s->local_port, ip, port);
    if (err == -EALREADY && s->nonblock)
        return err;
    if (err && err != -EALREADY)
        return err;
    s->remote_ip   = ip;
    s->remote_port = port;

    for (;;) {
        int r = tcp_connect_result(s->tcb);
        if (r != -EINPROGRESS) {
            s->connected = r == 0;
            return r;
        }
        if (s->nonblock)
            return -EINPROGRESS;
        err = sock_block(s, POLLOUT, 0);
        if (err)
            return err;
    }
}

int socket_disconnect(socket_t *s) {
    if (s->type == SOCK_STREAM)
        return -EOPNOTSUPP;
    s->connected   = false;
    s->remote_ip   = 0;
    s->remote_port = 0;
    return 0;
}

static long stream_send(socket_t *s, const void *buf, size_t len, int flags, bool user) {
    uint8_t *bounce = user ? kmalloc(len < SOCK_CHUNK ? len : SOCK_CHUNK) : NULL;
    if (user && !bounce && len)
        return -ENOMEM;

    size_t done = 0;
    long   err  = 0;
    while (done < len) {
        size_t      n   = len - done;
        const void *src = (const uint8_t *)buf + done;
        if (user) {
            if (n > SOCK_CHUNK) n = SOCK_CHUNK;
            if (copy_from_user(get_current_task(), bounce, src, n) != 0) {
                err = -EFAULT;
                break;
            }
            src = bounce;
        }

        long r = tcp_send(s->tcb, src, n);
        if (r > 0) {
            done += (size_t)r;
            continue;
        }
        if (r != -EAGAIN || s->nonblock || (flags & MSG_DONTWAIT)) {
            err = r;
            break;
        }
        err = sock_block(s, POLLOUT, 0);
        if (err)
            break;
    }

    kfree(bounce);
    return done ? (long)done : err;
}

static long stream_recv(socket_t *s, void *buf, size_t len, int flags, bool user) {
    uint64_t deadline = s->rcvtimeo_ms ? sched_deadline_ms(s->rcvtimeo_ms) : 0;
    if (len > SOCK_CHUNK && user)
        len = SOCK_CHUNK;

    uint8_t *bounce = user ? kmalloc(len ? len : 1) : NULL;
    if (user && !bounce)
        return -ENOMEM;

    long r;
    for (;;) {
        r = tcp_recv(s->tcb, user ? bounce : buf, len, (flags & MSG_PEEK) != 0);
        if (r != -EAGAIN || s->nonblock || (flags & MSG_DONTWAIT))
            break;
        int err = sock_block(s, POLLIN, deadline);
        if (err) {
            r = err;
            break;
        }
    }

    if (r > 0 && user && sock_copy_out(buf, bounce, (size_t)r, true) != 0)
        r = -EFAULT;
    kfree(bounce);
    return r;
}

static long sock_send(socket_t *s, const void *buf, size_t len, bool to_given, uint32_t ip, uint16_t port, bool user) {
    if (s->type == SOCK_STREAM)
        return to_given ? -EISCONN : stream_send(s, buf, len, 0, user);

    if (len > UDP_MAX_PAYLOAD)
        return -EMSGSIZE;

    if (!to_given) {
        if (!s->connected)
            return -EDESTADDRREQ;
        ip   = s->remote_ip;
        port = s->remote_port;
    }
    if (!port)
        return -EINVAL;

    netif_t *nif = netif_default();
    bool     subnet_bcast = nif && nif->netmask && ip == ((nif->ip & nif->netmask) | ~nif->netmask);
    if ((ip == IP_BROADCAST || subnet_bcast) && !s->broadcast)
        return -EACCES;

    int err = autobind(s);
    if (err)
        return err;

    // too big for one frame, fragment it
    if (len > UDP_FRAME_MAX) {
        uint8_t *flat = kmalloc(len);
        if (!flat)
            return -ENOMEM;
        if (user ? copy_from_user(get_current_task(), flat, buf, len) != 0 : (memcpy(flat, buf, len), false)) {
            kfree(flat);
            return -EFAULT;
        }
        err = udp_output_flat(NULL, flat, len, s->local_ip, s->local_port, ip, port);
        kfree(flat);
        return err ? err : (long)len;
    }

    netbuf_t *nb = netbuf_alloc();
    if (!nb)
        return -ENOBUFS;

    void *payload = netbuf_put(nb, len);
    if (user) {
        if (copy_from_user(get_current_task(), payload, buf, len) != 0) {
            netbuf_free(nb);
            return -EFAULT;
        }
    } else {
        memcpy(payload, buf, len);
    }

    err = udp_output(NULL, nb, s->local_ip, s->local_port, ip, port);
    return err ? err : (long)len;
}

static long sock_recv(socket_t *s, void *buf, size_t len, int flags, uint32_t *ip, uint16_t *port, bool user) {
    if (s->type == SOCK_STREAM) {
        if (ip)   *ip   = s->remote_ip;
        if (port) *port = s->remote_port;
        return stream_recv(s, buf, len, flags, user);
    }

    uint64_t deadline = s->rcvtimeo_ms ? sched_deadline_ms(s->rcvtimeo_ms) : 0;

    for (;;) {
        unsigned long irq = irq_push();
        spinlock_acquire(&sock_lock);

        sock_dgram_t *d = s->rx_head;
        if (d && !(flags & MSG_PEEK)) {
            s->rx_head = d->next;
            if (!s->rx_head)
                s->rx_tail = NULL;
            s->rx_count--;
        }

        // a peek leaves it queued
        sock_dgram_t *peeked = NULL;
        if (d && (flags & MSG_PEEK)) {
            peeked = kmalloc(sizeof(sock_dgram_t) + d->len);
            if (peeked)
                memcpy(peeked, d, sizeof(sock_dgram_t) + d->len);
            d = peeked;
        }

        spinlock_release(&sock_lock);
        irq_restore(irq);

        if (d) {
            size_t n   = len < d->len ? len : d->len;
            int    err = sock_copy_out(buf, d->data, n, user);
            if (ip)   *ip   = d->src;
            if (port) *port = d->port;
            kfree(d);
            return err ? err : (long)n;
        }
        if (flags & MSG_PEEK && s->rx_head)
            return -ENOMEM;

        if (s->nonblock || (flags & MSG_DONTWAIT))
            return -EAGAIN;

        int err = sock_block(s, POLLIN, deadline);
        if (err)
            return err;
    }
}

long socket_sendto(socket_t *s, const void *user_buf, size_t len, int flags, bool to_given, uint32_t ip, uint16_t port) {
    (void)flags;
    return sock_send(s, user_buf, len, to_given, ip, port, true);
}

long socket_recvfrom(socket_t *s, void *user_buf, size_t len, int flags, uint32_t *ip, uint16_t *port) {
    return sock_recv(s, user_buf, len, flags, ip, port, true);
}

void socket_name(socket_t *s, bool peer, uint32_t *ip, uint16_t *port) {
    *ip   = peer ? s->remote_ip : s->local_ip;
    *port = peer ? s->remote_port : s->local_port;
}

int socket_setopt(socket_t *s, int level, int name, const void *val, size_t len) {
    int on = 0;
    if (len >= sizeof(int))
        memcpy(&on, val, sizeof(int));

    if (level == SOL_SOCKET) {
        switch (name) {
            case SO_REUSEADDR: s->reuse = on != 0;     return 0;
            case SO_BROADCAST: s->broadcast = on != 0; return 0;
            case SO_RCVTIMEO: {
                sock_timeval_t tv;
                if (len < sizeof(tv))
                    return -EINVAL;
                memcpy(&tv, val, sizeof(tv));
                if (tv.sec < 0 || tv.usec < 0)
                    return -EINVAL;
                s->rcvtimeo_ms = (uint64_t)tv.sec * 1000 + (uint64_t)tv.usec / 1000;
                return 0;
            }
            // nothing here to size or time out, taken so callers dont give up
            case SO_SNDTIMEO:
            case SO_SNDBUF:
            case SO_RCVBUF:
            case SO_KEEPALIVE:
                return 0;
            default:
                return -ENOPROTOOPT;
        }
    }
    if (level == IPPROTO_IP && (name == IP_TOS || name == IP_MULTICAST_LOOP))
        return 0;
    // segments go out as soon as theres data anyway
    if (level == IPPROTO_TCP && name == TCP_NODELAY)
        return 0;
    return -ENOPROTOOPT;
}

int socket_getopt(socket_t *s, int level, int name, void *val, size_t *len) {
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;

    int out;
    switch (name) {
        case SO_TYPE:      out = s->type; break;
        case SO_ERROR:     out = s->type == SOCK_STREAM && s->tcb ? -tcp_take_error(s->tcb) : 0; break;
        case SO_REUSEADDR: out = s->reuse; break;
        case SO_BROADCAST: out = s->broadcast; break;
        case SO_RCVBUF:    out = SOCK_RX_MAX * SOCK_DGRAM_MAX; break;
        case SO_SNDBUF:    out = SOCK_DGRAM_MAX; break;
        default:           return -ENOPROTOOPT;
    }
    if (*len < sizeof(int))
        return -EINVAL;
    memcpy(val, &out, sizeof(int));
    *len = sizeof(int);
    return 0;
}

bool socket_udp_input(uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, const void *data, size_t len) {
    socket_t *to = NULL;
    bool      full = false;

    unsigned long flags = irq_push();
    spinlock_acquire(&sock_lock);

    for (socket_t *s = bound; s; s = s->next) {
        if (s->type != SOCK_DGRAM || s->local_port != dst_port)
            continue;
        if (s->local_ip && s->local_ip != dst && dst != IP_BROADCAST)
            continue;
        if (s->connected && (s->remote_ip != src || s->remote_port != src_port))
            continue;
        to = s;
        break;
    }

    if (to && to->rx_count >= SOCK_RX_MAX) {
        to->rx_dropped++;
        full = true;
    } else if (to) {
        sock_dgram_t *d = kmalloc(sizeof(sock_dgram_t) + len);
        if (d) {
            d->next = NULL;
            d->src  = src;
            d->port = src_port;
            d->len  = (uint16_t)len;
            memcpy(d->data, data, len);
            if (to->rx_tail)
                to->rx_tail->next = d;
            else
                to->rx_head = d;
            to->rx_tail = d;
            to->rx_count++;
        }
    }

    uint64_t dropped = to ? to->rx_dropped : 0;
    spinlock_release(&sock_lock);

    // still with interrupts off, a socket closing in between would free the queue
    if (to && !full)
        sched_wake_queue(&to->wait);
    irq_restore(flags);

    if (full && (dropped == 1 || dropped % 100 == 0))
        serial_printf(LOG_WARN "socket: port %u queue full, %llu datagrams dropped so far\n", dst_port, dropped);
    return to != NULL;
}

// fd ops, read and write are recv and send without the address

static long sock_read_inode(INode_t *inode, void *out, size_t size, size_t offset) {
    (void)offset;
    return sock_recv(inode->internal_data, out, size, 0, NULL, NULL, false);
}

static long sock_write_inode(INode_t *inode, const void *in, size_t size, size_t offset) {
    (void)offset;
    return sock_send(inode->internal_data, in, size, false, 0, 0, false);
}

static int sock_ioctl(INode_t *inode, unsigned long request, void *arg) {
    socket_t *s    = inode->internal_data;
    task_t   *task = get_current_task();
    if (!arg)
        return -EINVAL;

    switch (request) {
        case TTY_IOCTL_FIONBIO: {
            int nonblock;
            if (copy_from_user(task, &nonblock, arg, sizeof(nonblock)) != 0) return -EFAULT;
            s->nonblock = nonblock != 0;
            return 0;
        }
        // what fcntl O_NONBLOCK goes through in blibc
        case TTY_IOCTL_SET_FLAGS: {
            uint32_t flags;
            if (copy_from_user(task, &flags, arg, sizeof(flags)) != 0) return -EFAULT;
            s->nonblock = (flags & TTY_NONBLOCK) != 0;
            return 0;
        }
        case TTY_IOCTL_GET_FLAGS: {
            uint32_t flags = s->nonblock ? TTY_NONBLOCK : 0;
            return copy_to_user(task, arg, &flags, sizeof(flags)) == 0 ? 0 : -EFAULT;
        }
        case FIONREAD: {
            int next = s->type == SOCK_STREAM ? (int)tcp_pending(s->tcb) : s->rx_head ? s->rx_head->len : 0;
            return copy_to_user(task, arg, &next, sizeof(next)) == 0 ? 0 : -EFAULT;
        }
        default:
            return -ENOTTY;
    }
}

static int sock_poll(INode_t *inode) {
    return sock_poll_mask(inode->internal_data);
}

static wait_queue_t *sock_wait_queue(INode_t *inode) {
    socket_t *s = inode->internal_data;
    return &s->wait;
}

static const INodeOps_t sock_ops = {
    .read       = sock_read_inode,
    .write      = sock_write_inode,
    .ioctl      = sock_ioctl,
    .poll       = sock_poll,
    .wait_queue = sock_wait_queue,
};

int sockaddr_from_user(const void *user_addr, uint64_t len, uint32_t *ip, uint16_t *port) {
    sockaddr_in_t sa;
    if (!user_addr || len < sizeof(uint16_t))
        return -EINVAL;
    memset(&sa, 0, sizeof(sa));
    if (copy_from_user(get_current_task(), &sa, user_addr, len < sizeof(sa) ? len : sizeof(sa)) != 0)
        return -EFAULT;
    if (sa.family != AF_INET)
        return -EAFNOSUPPORT;
    if (len < sizeof(sa))
        return -EINVAL;

    *ip   = sa.addr;
    *port = ntohs(sa.port);
    return 0;
}

int sockaddr_to_user(void *user_addr, uint32_t *user_len, uint32_t ip, uint16_t port) {
    if (!user_addr || !user_len)
        return 0;

    task_t  *task = get_current_task();
    uint32_t room;
    if (copy_from_user(task, &room, user_len, sizeof(room)) != 0)
        return -EFAULT;

    sockaddr_in_t sa = { .family = AF_INET, .port = htons(port), .addr = ip };
    uint32_t      n  = room < sizeof(sa) ? room : sizeof(sa);
    uint32_t      full = sizeof(sa);
    if (copy_to_user(task, user_addr, &sa, n) != 0 || copy_to_user(task, user_len, &full, sizeof(full)) != 0)
        return -EFAULT;
    return 0;
}

int socket_create(int domain, int type, int protocol) {
    int kind = type & SOCK_TYPE_MASK;
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (kind != SOCK_DGRAM && kind != SOCK_STREAM)
        return -EPROTONOSUPPORT;
    if (protocol != 0 && protocol != (kind == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP))
        return -EPROTONOSUPPORT;

    socket_t *s     = kmalloc(sizeof(socket_t));
    INode_t  *inode = kmalloc(sizeof(INode_t));
    file_t   *f     = kmalloc(sizeof(file_t));
    if (!s || !inode || !f) {
        kfree(s);
        kfree(inode);
        kfree(f);
        return -ENOMEM;
    }

    memset(s, 0, sizeof(*s));
    memset(inode, 0, sizeof(*inode));
    memset(f, 0, sizeof(*f));
    s->nonblock = (type & SOCK_NONBLOCK) != 0;
    s->type     = kind;
    if (kind == SOCK_STREAM && !(s->tcb = tcp_create(&s->wait))) {
        kfree(s);
        kfree(inode);
        kfree(f);
        return -ENOMEM;
    }

    inode->type          = INODE_DEVICE;
    inode->ops           = &sock_ops;
    inode->internal_data = s;
    inode->shared        = 1;

    f->type   = FD_TYPE_SOCKET;
    f->inode  = inode;
    f->flags  = O_RDWR;
    f->shared = 1;

    int fd = vfs_install(f);
    if (fd < 0) {
        if (s->tcb)
            tcp_detach(s->tcb);
        kfree(s);
        kfree(inode);
        kfree(f);
    }
    return fd;
}

socket_t *socket_from_fd(int fd, int *err) {
    task_t *task = get_current_task();
    if (!task || !task->fd_table || fd < 0 || fd >= MAX_FDS || !task->fd_table->fds[fd]) {
        *err = -EBADF;
        return NULL;
    }

    file_t *f = task->fd_table->fds[fd];
    if (f->type != FD_TYPE_SOCKET || !f->inode) {
        *err = -ENOTSOCK;
        return NULL;
    }
    return f->inode->internal_data;
}

void socket_file_release(file_t *f) {
    socket_t *s = f->inode ? f->inode->internal_data : NULL;
    if (!s) {
        kfree(f->inode);
        return;
    }

    unsigned long flags = irq_push();
    spinlock_acquire(&sock_lock);
    for (socket_t **at = &bound; *at; at = &(*at)->next) {
        if (*at == s) {
            *at = s->next;
            break;
        }
    }
    sock_dgram_t *rx = s->rx_head;
    s->rx_head = s->rx_tail = NULL;
    spinlock_release(&sock_lock);
    irq_restore(flags);

    while (rx) {
        sock_dgram_t *next = rx->next;
        kfree(rx);
        rx = next;
    }
    if (s->tcb)
        tcp_detach(s->tcb);

    sched_wait_queue_destroy(&s->wait);
    kfree(f->inode);
    kfree(s);
}
