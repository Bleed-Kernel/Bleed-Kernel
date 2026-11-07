#include "tcp_priv.h"
#include <random/random.h>
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <sched/clock.h>
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
    uint32_t room = (TCP_RX_BUF - t->rx_len) >> t->rcv_shift;
    return (uint16_t)(room > 0xFFFF ? 0xFFFF : room);
}

void tcp_arm_rto(tcb_t *t) {
    if (!t->rto_at)
        t->rto_at = net_now_ms() + t->rto_ms;
}

// rfc 6528, a 4us clock plus a keyed hash of the connection so nobody off path can guess it
uint32_t tcp_new_iss(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port) {
    uint32_t tuple[4] = { local_ip, remote_ip, (uint32_t)local_port << 16 | remote_port, 0x7C9A0001 };
    uint32_t clock    = (uint32_t)(clock_monotonic_ns() / 4000ull);
    return clock + random_keyed32(tuple);
}

// a segment from scratch, data comes out of the tx ring starting off bytes past snd_una
static void tcp_segment(uint32_t src, uint32_t dst, uint16_t src_port, uint16_t dst_port,
                        uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                        const tcb_t *data_from, uint32_t off, uint32_t len,
                        const uint8_t *opts, size_t opt_len, tcp_out_t *out) {
    if (out->count >= TCP_OUT_MAX)
        return;

    netbuf_t *nb = netbuf_alloc();
    if (!nb)
        return;

    tcp_hdr_t *h = netbuf_put(nb, TCP_HLEN + opt_len + len);
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
    if (opt_len) {
        memcpy(p, opts, opt_len);
        p += opt_len;
    }

    // the ring can wrap in the middle of what goes out
    if (len) {
        uint32_t at    = (data_from->tx_start + off) % TCP_TX_BUF;
        uint32_t first = TCP_TX_BUF - at < len ? TCP_TX_BUF - at : len;
        memcpy(p, data_from->tx + at, first);
        memcpy(p + first, data_from->tx, len - first);
    }

    h->checksum = net_checksum(h, nb->len, ipv4_pseudo_sum(src, dst, IP_PROTO_TCP, nb->len));

    out->nb[out->count]  = nb;
    out->src[out->count] = src;
    out->dst[out->count] = dst;
    out->count++;
}

static void tcp_emit(tcb_t *t, uint8_t flags, uint32_t seq, uint32_t off, uint32_t len,
                     const uint8_t *opts, size_t opt_len, tcp_out_t *out) {
    uint16_t window = tcp_rcv_window(t);
    t->rcv_adv = t->rcv_nxt + ((uint32_t)window << t->rcv_shift);
    tcp_segment(t->local_ip, t->remote_ip, t->local_port, t->remote_port, seq, t->rcv_nxt, flags,
                window, t, off, len, opts, opt_len, out);
}

void tcp_send_ctl(tcb_t *t, uint8_t flags, uint32_t seq, tcp_out_t *out) {
    tcp_emit(t, flags, seq, 0, 0, NULL, 0, out);
}

// an ack, telling the peer what it doesnt need to resend when theres a hole
void tcp_send_ack(tcb_t *t, tcp_out_t *out) {
    uint8_t opts[TCP_SACK_OPTS];
    int     blocks = t->sack_ok ? (t->ooo_count < 3 ? t->ooo_count : 3) : 0;
    size_t  len    = 0;

    if (blocks) {
        opts[0] = TCP_OPT_NOP;
        opts[1] = TCP_OPT_NOP;
        opts[2] = TCP_OPT_SACK;
        opts[3] = (uint8_t)(2 + blocks * 8);
        len     = 4;
        for (int i = 0; i < blocks; i++) {
            uint32_t start = htonl(t->ooo[i].start);
            uint32_t end   = htonl(t->ooo[i].end);
            memcpy(opts + len, &start, 4);
            memcpy(opts + len + 4, &end, 4);
            len += 8;
        }
    }
    tcp_emit(t, TCP_ACK, t->snd_nxt, 0, 0, opts, len, out);
}

void tcp_send_syn(tcb_t *t, tcp_out_t *out) {
    uint8_t opts[TCP_SYN_OPTS] = {
        TCP_OPT_MSS, 4, TCP_MSS >> 8, TCP_MSS & 0xFF,
        TCP_OPT_NOP, TCP_OPT_WSCALE, 3, TCP_WSCALE,
        TCP_OPT_NOP, TCP_OPT_NOP, TCP_OPT_SACK_OK, 2,
    };
    uint8_t flags = t->state == TCP_SYN_RCVD ? TCP_SYN | TCP_ACK : TCP_SYN;
    tcp_emit(t, flags, t->iss, 0, 0, opts, sizeof(opts), out);
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
    tcp_segment(src, dst, ntohs(in->dst_port), ntohs(in->src_port), seq, ack, flags, 0, NULL, 0, 0, NULL, 0, out);
}

// resend whatever starts at seq, how much sequence space that covered. 0 when theres nothing there
static uint32_t tcp_resend(tcb_t *t, uint32_t seq, tcp_out_t *out) {
    uint32_t off      = seq - t->snd_una;
    uint32_t data_end = t->snd_nxt - t->snd_una - (t->fin_sent ? 1 : 0);

    t->rtt_at = 0;  // karn, cant tell which copy an ack is for
    if (off < data_end) {
        uint32_t n = data_end - off < t->mss ? data_end - off : t->mss;
        tcp_emit(t, TCP_ACK | TCP_PSH, seq, off, n, NULL, 0, out);
        return n;
    }
    if (t->fin_sent && seq == t->snd_nxt - 1) {
        tcp_send_ctl(t, TCP_FIN | TCP_ACK, seq, out);
        return 1;
    }
    return 0;
}

static void tcp_push_recovery(tcb_t *t, tcp_out_t *out) {
    if (t->recovery == TCP_RECOVERY_FAST) {
        if (t->rtx_one) {
            t->rtx_one = false;
            tcp_resend(t, t->snd_una, out);
        }
        return;
    }
    if (t->recovery != TCP_RECOVERY_RTO)
        return;

    if (SEQ_LT(t->rtx_nxt, t->snd_una))
        t->rtx_nxt = t->snd_una;
    while (SEQ_LT(t->rtx_nxt, t->recover) && t->rtx_nxt - t->snd_una < t->cwnd && out->count < TCP_OUT_MAX) {
        uint32_t n = tcp_resend(t, t->rtx_nxt, out);
        if (!n)
            break;
        t->rtx_nxt += n;
    }
}

void tcp_push(tcb_t *t, tcp_out_t *out) {
    tcp_push_recovery(t, out);
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
        return;

    uint32_t sent = t->snd_nxt - t->snd_una;
    uint32_t wnd  = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;

    while (sent < t->tx_len && out->count < TCP_OUT_MAX) {
        uint32_t room = wnd > sent ? wnd - sent : 0;
        if (!room)
            break;

        uint32_t n = t->tx_len - sent;
        if (n > t->mss) n = t->mss;
        if (n > room) {
            // wait for a proper segment while acks are still due
            if (sent)
                break;
            n = room;
        }

        if (!t->rtt_at && t->recovery == TCP_RECOVERY_NONE) {
            t->rtt_at  = net_now_ms();
            t->rtt_seq = t->snd_nxt;
        }

        tcp_emit(t, TCP_ACK | TCP_PSH, t->snd_nxt, sent, n, NULL, 0, out);
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

    if (t->tx_len && !sent && !t->snd_wnd)
        tcp_arm_rto(t);
}

void tcp_set_closed(tcb_t *t, int err) {
    if (err && !t->error)
        t->error = err;
    t->state    = TCP_CLOSED;
    t->rto_at   = 0;
    t->recovery = TCP_RECOVERY_NONE;
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

// a half open connection that never made it, the tick frees it
void tcp_drop_child(tcb_t *t) {
    if (t->parent)
        t->parent->pending--;
    t->parent   = NULL;
    t->detached = true;
    t->state    = TCP_CLOSED;
    t->rto_at   = 0;
}

// starts over from the oldest thing nobody acked
static void tcp_enter_rto(tcb_t *t) {
    uint32_t flight = t->snd_nxt - t->snd_una;
    uint32_t half   = flight / 2;

    t->ssthresh = half > 2u * t->mss ? half : 2u * t->mss;
    t->cwnd     = t->mss;
    t->dupacks  = 0;
    t->rtx_one  = false;
    t->recovery = TCP_RECOVERY_RTO;
    t->recover  = t->snd_nxt;
    t->rtx_nxt  = t->snd_una;
    t->rtt_at   = 0;
}

static void tcp_timeout(tcb_t *t, uint64_t now, tcp_out_t *out) {
    uint32_t flight = t->snd_nxt - t->snd_una;
    bool     probe  = !flight && t->tx_len && !t->snd_wnd;
    bool     syn    = t->state == TCP_SYN_SENT || t->state == TCP_SYN_RCVD;

    // the window opened by itself, nothing was lost
    if (!syn && !flight && !probe) {
        t->rto_at = 0;
        return;
    }

    int limit = syn ? TCP_SYN_RETRIES : TCP_RETRIES;
    if (++t->retries > limit) {
        serial_printf(LOG_WARN "tcp: " IP_FMT ":%u stopped answering, giving up\n",
                      IP_ARGS(t->remote_ip), t->remote_port);
        if (t->state == TCP_SYN_RCVD) {
            tcp_drop_child(t);
            return;
        }
        if (t->state != TCP_SYN_SENT)
            tcp_send_ctl(t, TCP_RST, t->snd_nxt, out);
        tcp_set_closed(t, -ETIMEDOUT);
        return;
    }

    if (syn) {
        tcp_send_syn(t, out);
    } else if (probe) {
        // a byte past the shut window asks whether its open again
        tcp_emit(t, TCP_ACK, t->snd_nxt, 0, 1, NULL, 0, out);
        t->snd_nxt++;
    } else {
        tcp_enter_rto(t);
    }

    t->rto_ms = t->rto_ms * 2 > TCP_RTO_MAX_MS ? TCP_RTO_MAX_MS : t->rto_ms * 2;
    t->rto_at = now + t->rto_ms;
}

static void tcp_keepalive(tcb_t *t, uint64_t now, tcp_out_t *out) {
    if (!t->keepalive || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT))
        return;
    if (t->snd_una != t->snd_nxt)
        return;

    uint64_t next = t->last_rx + ((uint64_t)t->keep_idle + (uint64_t)t->keep_sent * t->keep_intvl) * 1000;
    if (now < next)
        return;

    if (t->keep_sent >= t->keep_cnt) {
        serial_printf(LOG_INFO "tcp: " IP_FMT ":%u went quiet on keepalive\n", IP_ARGS(t->remote_ip), t->remote_port);
        tcp_send_ctl(t, TCP_RST, t->snd_nxt, out);
        tcp_set_closed(t, -ETIMEDOUT);
        return;
    }
    t->keep_sent++;
    tcp_send_ctl(t, TCP_ACK, t->snd_nxt - 1, out);
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
        tcp_keepalive(t, now, &out);

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
