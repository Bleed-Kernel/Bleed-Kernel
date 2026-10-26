#include "tcp_priv.h"
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

spinlock_t tcp_lock;
tcb_t     *tcp_list;

void tcp_out_flush(tcp_out_t *out) {
    for (int i = 0; i < out->count; i++)
        ipv4_output(NULL, out->nb[i], out->src[i], out->dst[i], IP_PROTO_TCP);
    out->count = 0;
}

uint16_t tcp_rcv_window(const tcb_t *t) {
    uint32_t room = TCP_BUF - t->rx_len;
    return (uint16_t)(room > 0xFFFF ? 0xFFFF : room);
}

void tcp_arm_rto(tcb_t *t) {
    if (!t->rto_at)
        t->rto_at = net_now_ms() + t->rto_ms;
}

// a segment from scratch, data comes out of the tx ring starting off bytes past snd_una
static void tcp_segment(uint32_t src, uint32_t dst, uint16_t src_port, uint16_t dst_port,
                        uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                        const tcb_t *data_from, uint32_t off, uint32_t len, bool mss_opt, tcp_out_t *out) {
    if (out->count >= TCP_OUT_MAX)
        return;

    netbuf_t *nb = netbuf_alloc();
    if (!nb)
        return;

    size_t     opt_len = mss_opt ? 4 : 0;
    tcp_hdr_t *h       = netbuf_put(nb, TCP_HLEN + opt_len + len);
    h->src_port = htons(src_port);
    h->dst_port = htons(dst_port);
    h->seq      = htonl(seq);
    h->ack      = htonl(ack);
    h->off      = (uint8_t)(((TCP_HLEN + opt_len) / 4) << 4);
    h->flags    = flags;
    h->window   = htons(window);
    h->checksum = 0;
    h->urgent   = 0;

    uint8_t *p = (uint8_t *)(h + 1);
    if (mss_opt) {
        p[0] = TCP_OPT_MSS;
        p[1] = 4;
        p[2] = TCP_MSS >> 8;
        p[3] = TCP_MSS & 0xFF;
        p += 4;
    }

    // the ring can wrap in the middle of what goes out
    if (len) {
        uint32_t at    = (data_from->tx_start + off) % TCP_BUF;
        uint32_t first = TCP_BUF - at < len ? TCP_BUF - at : len;
        memcpy(p, data_from->tx + at, first);
        memcpy(p + first, data_from->tx, len - first);
    }

    h->checksum = net_checksum(h, nb->len, ipv4_pseudo_sum(src, dst, IP_PROTO_TCP, nb->len));

    out->nb[out->count]  = nb;
    out->src[out->count] = src;
    out->dst[out->count] = dst;
    out->count++;
}

void tcp_send_ctl(tcb_t *t, uint8_t flags, uint32_t seq, tcp_out_t *out) {
    tcp_segment(t->local_ip, t->remote_ip, t->local_port, t->remote_port, seq, t->rcv_nxt, flags,
                tcp_rcv_window(t), NULL, 0, 0, false, out);
}

void tcp_send_syn(tcb_t *t, tcp_out_t *out) {
    tcp_segment(t->local_ip, t->remote_ip, t->local_port, t->remote_port, t->iss, 0, TCP_SYN,
                tcp_rcv_window(t), NULL, 0, 0, true, out);
}

void tcp_reset_reply(uint32_t src, uint32_t dst, const tcp_hdr_t *in, size_t dlen, tcp_out_t *out) {
    uint32_t seq = 0, ack = 0;
    uint8_t  flags = TCP_RST;

    if (in->flags & TCP_ACK) {
        seq = ntohl(in->ack);
    } else {
        ack    = ntohl(in->seq) + (uint32_t)dlen + ((in->flags & TCP_SYN) ? 1 : 0) + ((in->flags & TCP_FIN) ? 1 : 0);
        flags |= TCP_ACK;
    }
    tcp_segment(src, dst, ntohs(in->dst_port), ntohs(in->src_port), seq, ack, flags, 0, NULL, 0, 0, false, out);
}

void tcp_push(tcb_t *t, tcp_out_t *out) {
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
        return;

    uint32_t sent = t->snd_nxt - t->snd_una;
    while (sent < t->tx_len && out->count < TCP_OUT_MAX) {
        uint32_t room = t->snd_wnd > sent ? t->snd_wnd - sent : 0;
        if (!room)
            break;

        uint32_t n = t->tx_len - sent;
        if (n > t->mss) n = t->mss;
        if (n > room)   n = room;

        tcp_segment(t->local_ip, t->remote_ip, t->local_port, t->remote_port, t->snd_nxt, t->rcv_nxt,
                    TCP_ACK | TCP_PSH, tcp_rcv_window(t), t, sent, n, false, out);
        t->snd_nxt += n;
        sent       += n;
        tcp_arm_rto(t);
    }

    if (t->fin_wanted && !t->fin_sent && sent == t->tx_len) {
        tcp_send_ctl(t, TCP_FIN | TCP_ACK, t->snd_nxt, out);
        t->snd_nxt++;
        t->fin_sent = true;
        t->state    = t->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
        tcp_arm_rto(t);
    }
}

void tcp_set_closed(tcb_t *t, int err) {
    if (err && !t->error)
        t->error = err;
    t->state  = TCP_CLOSED;
    t->rto_at = 0;
    if (t->wait)
        sched_wake_queue(t->wait);
}

void tcp_unlink(tcb_t *t) {
    for (tcb_t **at = &tcp_list; *at; at = &(*at)->next) {
        if (*at == t) {
            *at = t->next;
            break;
        }
    }
    t->linked = false;
}

void tcp_free(tcb_t *t) {
    if (t->linked)
        tcp_unlink(t);
    kfree(t->rx);
    kfree(t->tx);
    kfree(t);
}

// the oldest thing nobody acked goes again
static void tcp_retransmit(tcb_t *t, tcp_out_t *out) {
    if (t->state == TCP_SYN_SENT) {
        tcp_send_syn(t, out);
        return;
    }

    uint32_t in_flight = t->snd_nxt - t->snd_una - (t->fin_sent ? 1 : 0);
    if (in_flight) {
        uint32_t n = in_flight < t->mss ? in_flight : t->mss;
        tcp_segment(t->local_ip, t->remote_ip, t->local_port, t->remote_port, t->snd_una, t->rcv_nxt,
                    TCP_ACK | TCP_PSH, tcp_rcv_window(t), t, 0, n, false, out);
    } else if (t->fin_sent) {
        tcp_send_ctl(t, TCP_FIN | TCP_ACK, t->snd_nxt - 1, out);
    } else if (t->tx_len && !t->snd_wnd) {
        // a shut window, a byte past it asks whether its open again
        tcp_segment(t->local_ip, t->remote_ip, t->local_port, t->remote_port, t->snd_nxt, t->rcv_nxt,
                    TCP_ACK, tcp_rcv_window(t), t, 0, 1, false, out);
        t->snd_nxt++;
    }
}

static void tcp_timeout(tcb_t *t, uint64_t now, tcp_out_t *out) {
    int limit = t->state == TCP_SYN_SENT ? TCP_SYN_RETRIES : TCP_RETRIES;
    if (++t->retries > limit) {
        serial_printf(LOG_WARN "tcp: " IP_FMT ":%u stopped answering, giving up\n",
                      IP_ARGS(t->remote_ip), t->remote_port);
        if (t->state != TCP_SYN_SENT)
            tcp_send_ctl(t, TCP_RST, t->snd_nxt, out);
        tcp_set_closed(t, -ETIMEDOUT);
        return;
    }

    tcp_retransmit(t, out);
    t->rto_ms = t->rto_ms * 2 > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : t->rto_ms * 2;
    t->rto_at = now + t->rto_ms;
}

void tcp_tick(void) {
    tcp_out_t out = { .count = 0 };
    uint64_t  now = net_now_ms();

    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    tcb_t *t = tcp_list;
    while (t) {
        tcb_t *next = t->next;

        if (t->rto_at && now >= t->rto_at)
            tcp_timeout(t, now, &out);

        // the window opened while nothing was in flight to carry the news
        tcp_push(t, &out);

        if (t->state == TCP_TIME_WAIT && now >= t->done_at)
            tcp_set_closed(t, 0);
        if (t->detached && t->state != TCP_CLOSED && t->done_at && now >= t->done_at) {
            tcp_send_ctl(t, TCP_RST | TCP_ACK, t->snd_nxt, &out);
            tcp_set_closed(t, 0);
        }
        if (t->detached && t->state == TCP_CLOSED)
            tcp_free(t);
        else if (t->state == TCP_CLOSED && t->linked)
            tcp_unlink(t);

        t = next;
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
}
