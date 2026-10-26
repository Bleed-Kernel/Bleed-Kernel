#include "tcp_priv.h"
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <mm/kalloc.h>
#include <user/user_poll.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

// client side only for now, nothing listens. enough for http and whatever else dials out

tcb_t *tcp_create(wait_queue_t *wait) {
    tcb_t *t = kmalloc(sizeof(tcb_t));
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    t->rx = kmalloc(TCP_BUF);
    t->tx = kmalloc(TCP_BUF);
    if (!t->rx || !t->tx) {
        kfree(t->rx);
        kfree(t->tx);
        kfree(t);
        return NULL;
    }

    t->state  = TCP_CLOSED;
    t->mss    = TCP_MSS_DEFAULT;
    t->rto_ms = TCP_RTO_MS;
    t->wait   = wait;
    return t;
}

int tcp_connect(tcb_t *t, uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    netif_t *nif = ipv4_route(remote_ip);
    if (!nif)
        return -ENETUNREACH;

    tcp_out_t out = { .count = 0 };
    int       err = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    if (t->state == TCP_SYN_SENT) {
        err = -EALREADY;
    } else if (t->state != TCP_CLOSED || t->was_connected) {
        err = -EISCONN;
    } else {
        t->local_ip    = local_ip ? local_ip : ipv4_source(nif, remote_ip);
        t->local_port  = local_port;
        t->remote_ip   = remote_ip;
        t->remote_port = remote_port;
        t->iss         = (uint32_t)hpet_read_counter() ^ ((uint32_t)local_port << 16);
        t->snd_una     = t->iss;
        t->snd_nxt     = t->iss + 1;
        t->state       = TCP_SYN_SENT;
        t->error       = 0;
        tcp_send_syn(t, &out);
        tcp_arm_rto(t);

        t->next   = tcp_list;
        tcp_list  = t;
        t->linked = true;
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
        uint32_t room = TCP_BUF - t->tx_len;
        uint32_t n    = len < room ? (uint32_t)len : room;
        uint32_t at   = (t->tx_start + t->tx_len) % TCP_BUF;
        uint32_t first = TCP_BUF - at < n ? TCP_BUF - at : n;

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
        uint32_t first = TCP_BUF - t->rx_start < n ? TCP_BUF - t->rx_start : n;
        memcpy(buf, t->rx + t->rx_start, first);
        memcpy((uint8_t *)buf + first, t->rx, n - first);

        if (!peek) {
            uint16_t before = tcp_rcv_window(t);
            t->rx_start = (t->rx_start + n) % TCP_BUF;
            t->rx_len  -= n;
            // tell a stalled sender once theres a segments worth of room
            if (before < TCP_MSS && tcp_rcv_window(t) >= TCP_MSS && t->state != TCP_CLOSED)
                tcp_send_ctl(t, TCP_ACK, t->snd_nxt, &out);
        }
        r = n;
    } else if (t->peer_fin) {
        r = 0;
    } else if (t->error) {
        r = t->error;
        t->error = 0;
    } else if (t->state == TCP_CLOSED) {
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
    if (t->rx_len || t->peer_fin)
        ready |= POLLIN;
    if ((t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) && !t->fin_wanted && t->tx_len < TCP_BUF)
        ready |= POLLOUT;
    if (t->error)
        ready |= POLLERR | POLLIN;
    if (t->state == TCP_CLOSED && t->was_connected)
        ready |= POLLHUP | POLLIN;

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

void tcp_detach(tcb_t *t) {
    tcp_out_t out = { .count = 0 };

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    t->wait     = NULL;
    t->detached = true;

    if (t->state == TCP_CLOSED || t->state == TCP_SYN_SENT) {
        tcp_free(t);
    } else if (t->rx_len && !t->fin_sent) {
        // unread data on close means reset
        tcp_send_ctl(t, TCP_RST | TCP_ACK, t->snd_nxt, &out);
        tcp_free(t);
    } else {
        t->fin_wanted = true;
        t->done_at    = net_now_ms() + TCP_LINGER_MS;
        tcp_push(t, &out);
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
}
