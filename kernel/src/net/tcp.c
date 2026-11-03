#include "tcp_priv.h"
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <user/user_poll.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

tcb_t *tcp_alloc(void) {
    tcb_t *t = kmalloc(sizeof(tcb_t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    t->state      = TCP_CLOSED;
    t->mss        = TCP_MSS_DEFAULT;
    t->rto_ms     = TCP_RTO_MS;
    t->rto_base   = TCP_RTO_MS;
    t->keep_idle  = TCP_KEEP_IDLE_S;
    t->keep_intvl = TCP_KEEP_INTVL_S;
    t->keep_cnt   = TCP_KEEP_CNT;
    return t;
}

int tcp_alloc_rings(tcb_t *t) {
    t->rx = kmalloc(TCP_RX_BUF);
    t->tx = kmalloc(TCP_TX_BUF);
    if (t->rx && t->tx)
        return 0;

    kfree(t->rx);
    kfree(t->tx);
    t->rx = t->tx = NULL;
    return -ENOMEM;
}

tcb_t *tcp_create(wait_queue_t *wait) {
    tcb_t *t = tcp_alloc();
    if (!t)
        return NULL;
    if (tcp_alloc_rings(t) != 0) {
        kfree(t);
        return NULL;
    }
    t->wait = wait;
    return t;
}

int tcp_connect(tcb_t *t, uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    netif_t *nif = ipv4_route(remote_ip);
    if (!nif)
        return -ENETUNREACH;

    uint32_t  src = local_ip ? local_ip : ipv4_source(nif, remote_ip);
    tcp_out_t out = { .count = 0 };
    int       err = 0;

    if (!src)
        return -ENETUNREACH;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    if (t->state == TCP_SYN_SENT) {
        err = -EALREADY;
    } else if (t->state != TCP_CLOSED || t->was_connected) {
        err = -EISCONN;
    } else {
        t->local_ip    = src;
        t->local_port  = local_port;
        t->remote_ip   = remote_ip;
        t->remote_port = remote_port;
        t->iss         = tcp_new_iss(src, local_port, remote_ip, remote_port);
        t->snd_una     = t->iss;
        t->snd_nxt     = t->iss + 1;
        t->state       = TCP_SYN_SENT;
        t->error       = 0;
        t->retries     = 0;
        t->rto_ms      = t->rto_base;
        t->rto_at      = 0;
        tcp_send_syn(t, &out);
        tcp_arm_rto(t);

        // a refused try may still be waiting for the tick to take it off
        if (!t->linked) {
            t->next   = tcp_list;
            tcp_list  = t;
            t->linked = true;
        }
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);

    if (!err)
        serial_printf(LOG_INFO "tcp: connecting to " IP_FMT ":%u from port %u\n", IP_ARGS(remote_ip), remote_port, local_port);
    return err;
}

int tcp_connect_result(tcb_t *t) {
    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    int r;
    if (t->state == TCP_SYN_SENT)
        r = -EINPROGRESS;
    else if (t->was_connected)
        r = 0;
    else
        r = t->error ? t->error : -ECONNREFUSED;

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    return r;
}

int tcp_listen(tcb_t *t, uint32_t local_ip, uint16_t local_port, int backlog) {
    if (backlog < 1)
        backlog = 1;
    if (backlog > TCP_BACKLOG_MAX)
        backlog = TCP_BACKLOG_MAX;

    int err = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    if (t->state == TCP_LISTEN) {
        t->backlog = backlog;
    } else if (t->state != TCP_CLOSED || t->was_connected) {
        err = -EISCONN;
    } else {
        t->local_ip   = local_ip;
        t->local_port = local_port;
        t->backlog    = backlog;
        t->state      = TCP_LISTEN;

        if (!t->linked) {
            t->next   = tcp_list;
            tcp_list  = t;
            t->linked = true;
        }

        // connections get their own, a listener would only sit on them
        kfree(t->rx);
        kfree(t->tx);
        t->rx = t->tx = NULL;
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);

    if (!err)
        serial_printf(LOG_INFO "tcp: listening on " IP_FMT ":%u\n", IP_ARGS(local_ip), local_port);
    return err;
}

int tcp_accept(tcb_t *t, wait_queue_t *wait, tcb_t **child) {
    int err = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    tcb_t *c = t->accept_head;
    if (t->state != TCP_LISTEN) {
        err = -EINVAL;
    } else if (!c) {
        err = -EAGAIN;
    } else {
        t->accept_head = c->accept_next;
        if (!t->accept_head)
            t->accept_tail = NULL;
        t->pending--;

        c->accept_next = NULL;
        c->parent      = NULL;
        c->wait        = wait;
        *child         = c;
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    return err;
}

void tcp_endpoints(tcb_t *t, uint32_t *local_ip, uint16_t *local_port, uint32_t *remote_ip, uint16_t *remote_port) {
    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);
    *local_ip    = t->local_ip;
    *local_port  = t->local_port;
    *remote_ip   = t->remote_ip;
    *remote_port = t->remote_port;
    spinlock_release(&tcp_lock);
    irq_restore(flags);
}

int tcp_shutdown(tcb_t *t, bool rd, bool wr) {
    tcp_out_t out = { .count = 0 };
    int       err = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    if (!t->was_connected || t->state == TCP_CLOSED) {
        err = -ENOTCONN;
    } else {
        if (rd)
            t->rd_shut = true;
        if (wr && !t->fin_wanted) {
            t->fin_wanted = true;
            tcp_push(t, &out);
        }
        if (t->wait)
            sched_wake_queue(t->wait);
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
    return err;
}

long tcp_send(tcb_t *t, const void *buf, size_t len) {
    tcp_out_t out = { .count = 0 };
    long      r;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    if (t->error) {
        r = t->error;
    } else if (t->state == TCP_SYN_SENT) {
        r = -EAGAIN;
    } else if ((t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT) || t->fin_wanted) {
        r = t->was_connected ? -EPIPE : -ENOTCONN;
    } else {
        uint32_t room = TCP_TX_BUF - t->tx_len;
        uint32_t n    = len < room ? (uint32_t)len : room;
        uint32_t at   = (t->tx_start + t->tx_len) % TCP_TX_BUF;
        uint32_t first = TCP_TX_BUF - at < n ? TCP_TX_BUF - at : n;

        memcpy(t->tx + at, buf, first);
        memcpy(t->tx, (const uint8_t *)buf + first, n - first);
        t->tx_len += n;
        tcp_push(t, &out);
        r = n ? (long)n : -EAGAIN;
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
    return r;
}

long tcp_recv(tcb_t *t, void *buf, size_t len, bool peek) {
    tcp_out_t out = { .count = 0 };
    long      r;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    if (t->rx_len) {
        uint32_t n     = len < t->rx_len ? (uint32_t)len : t->rx_len;
        uint32_t first = TCP_RX_BUF - t->rx_start < n ? TCP_RX_BUF - t->rx_start : n;
        memcpy(buf, t->rx + t->rx_start, first);
        memcpy((uint8_t *)buf + first, t->rx, n - first);

        if (!peek) {
            t->rx_start = (t->rx_start + n) % TCP_RX_BUF;
            t->rx_len  -= n;

            // tell a sender thats nearly run out of window, once its worth it
            uint32_t edge = t->rcv_nxt + (TCP_RX_BUF - t->rx_len);
            uint32_t told = t->rcv_adv - t->rcv_nxt;
            if (told < TCP_RX_BUF / 4 && (int32_t)(edge - t->rcv_adv) >= 2 * TCP_MSS && t->state != TCP_CLOSED)
                tcp_send_ctl(t, TCP_ACK, t->snd_nxt, &out);
        }
        r = n;
    } else if (t->peer_fin || t->rd_shut) {
        r = 0;
    } else if (t->error) {
        r = t->error;
        t->error = 0;
    } else if (t->state == TCP_CLOSED || t->state == TCP_LISTEN) {
        r = t->was_connected ? 0 : -ENOTCONN;
    } else {
        r = -EAGAIN;
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
    return r;
}

int tcp_poll(tcb_t *t) {
    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    int ready = 0;
    if (t->state == TCP_LISTEN) {
        if (t->accept_head)
            ready |= POLLIN;
    } else if (t->state == TCP_CLOSED) {
        // nothing will ever block here again, a failed connect has to show up too
        ready |= POLLHUP | POLLOUT;
        if (t->was_connected)
            ready |= POLLIN;
    } else {
        if (t->rx_len || t->peer_fin || t->rd_shut)
            ready |= POLLIN;
        if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) && !t->fin_wanted && t->tx_len < TCP_TX_BUF)
            ready |= POLLOUT;
    }
    if (t->error)
        ready |= POLLERR | POLLIN | POLLOUT;

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    return ready;
}

int tcp_take_error(tcb_t *t) {
    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);
    int err = t->error;
    t->error = 0;
    spinlock_release(&tcp_lock);
    irq_restore(flags);
    return err;
}

size_t tcp_pending(tcb_t *t) {
    return t->rx_len;
}

int tcp_setopt(tcb_t *t, int name, int val) {
    switch (name) {
        // segments go out as soon as theres data anyway
        case TCP_NODELAY: t->nodelay = val != 0; return 0;
        case TCP_KEEPIDLE:
            if (val < 1) return -EINVAL;
            t->keep_idle = (uint32_t)val;
            return 0;
        case TCP_KEEPINTVL:
            if (val < 1) return -EINVAL;
            t->keep_intvl = (uint32_t)val;
            return 0;
        case TCP_KEEPCNT:
            if (val < 1) return -EINVAL;
            t->keep_cnt = (uint32_t)val;
            return 0;
        case TCP_MAXSEG:
            return 0;
        default:
            return -ENOPROTOOPT;
    }
}

int tcp_getopt(tcb_t *t, int name, int *val) {
    switch (name) {
        case TCP_NODELAY:   *val = t->nodelay; return 0;
        case TCP_KEEPIDLE:  *val = (int)t->keep_idle; return 0;
        case TCP_KEEPINTVL: *val = (int)t->keep_intvl; return 0;
        case TCP_KEEPCNT:   *val = (int)t->keep_cnt; return 0;
        case TCP_MAXSEG:    *val = t->mss; return 0;
        default:            return -ENOPROTOOPT;
    }
}

void tcp_set_keepalive(tcb_t *t, bool on) {
    t->keepalive = on;
    t->keep_sent = 0;
    t->last_rx   = net_now_ms();
}

bool tcp_get_keepalive(tcb_t *t) {
    return t->keepalive;
}

bool tcp_is_listening(tcb_t *t) {
    return t->state == TCP_LISTEN;
}

// everything still waiting for an accept that wont come
static void tcp_listener_gone(tcb_t *l, tcp_out_t *out) {
    tcb_t *c = l->accept_head;
    while (c) {
        tcb_t *next = c->accept_next;
        if (c->state != TCP_CLOSED)
            tcp_send_ctl(c, TCP_RST | TCP_ACK, c->snd_nxt, out);
        tcp_free(c);
        c = next;
    }

    for (tcb_t *t = tcp_list; t; t = t->next) {
        if (t->parent == l) {
            tcp_send_ctl(t, TCP_RST | TCP_ACK, t->snd_nxt, out);
            tcp_drop_child(t);
        }
    }
}

void tcp_detach(tcb_t *t, bool abort) {
    tcp_out_t out = { .count = 0 };

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    t->wait     = NULL;
    t->detached = true;

    if (t->state == TCP_LISTEN) {
        tcp_listener_gone(t, &out);
        tcp_free(t);
    } else if (t->state == TCP_CLOSED || t->state == TCP_SYN_SENT) {
        tcp_free(t);
    } else if (abort || (t->rx_len && !t->fin_sent)) {
        // unread data on close means reset
        tcp_send_ctl(t, TCP_RST | TCP_ACK, t->snd_nxt, &out);
        tcp_free(t);
    } else if (t->state != TCP_TIME_WAIT) {
        t->fin_wanted = true;
        t->done_at    = net_now_ms() + TCP_LINGER_MS;
        tcp_push(t, &out);
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
}
