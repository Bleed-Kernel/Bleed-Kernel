#include "tcp_priv.h"
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

static tcb_t *tcp_find(uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port) {
    for (tcb_t *t = tcp_list; t; t = t->next) {
        if (t->local_port == dst_port && t->remote_port == src_port && t->remote_ip == src &&
            (!t->local_ip || t->local_ip == dst))
            return t;
    }
    return NULL;
}

static uint16_t tcp_parse_mss(const tcp_hdr_t *h, size_t hlen) {
    const uint8_t *p   = (const uint8_t *)h + TCP_HLEN;
    const uint8_t *end = (const uint8_t *)h + hlen;

    while (p < end && *p != TCP_OPT_END) {
        if (*p == TCP_OPT_NOP) {
            p++;
            continue;
        }
        if (p + 2 > end || p[1] < 2 || p + p[1] > end)
            break;
        if (p[0] == TCP_OPT_MSS && p[1] == 4)
            return (uint16_t)(p[2] << 8 | p[3]);
        p += p[1];
    }
    return TCP_MSS_DEFAULT;
}

static void tcp_syn_sent(tcb_t *t, const tcp_hdr_t *h, size_t hlen, uint32_t seq, uint32_t ack, tcp_out_t *out) {
    if ((h->flags & TCP_ACK) && ack != t->snd_nxt) {
        if (!(h->flags & TCP_RST))
            tcp_send_ctl(t, TCP_RST, ack, out);
        return;
    }

    if (h->flags & TCP_RST) {
        if (h->flags & TCP_ACK) {
            serial_printf(LOG_INFO "tcp: " IP_FMT ":%u refused the connection\n", IP_ARGS(t->remote_ip), t->remote_port);
            tcp_set_closed(t, -ECONNREFUSED);
        }
        return;
    }

    if (!(h->flags & TCP_SYN) || !(h->flags & TCP_ACK))
        return;

    uint16_t mss = tcp_parse_mss(h, hlen);
    t->mss           = mss < TCP_MSS ? mss : TCP_MSS;
    t->rcv_nxt       = seq + 1;
    t->snd_una       = ack;
    t->snd_wnd       = ntohs(h->window);
    t->state         = TCP_ESTABLISHED;
    t->was_connected = true;
    t->rto_at        = 0;
    t->rto_ms        = TCP_RTO_MS;
    t->retries       = 0;

    serial_printf(LOG_INFO "tcp: connected to " IP_FMT ":%u from port %u, mss %u\n",
                  IP_ARGS(t->remote_ip), t->remote_port, t->local_port, t->mss);
    tcp_send_ctl(t, TCP_ACK, t->snd_nxt, out);
    if (t->wait)
        sched_wake_queue(t->wait);
}

static void tcp_got_ack(tcb_t *t, uint32_t ack, uint16_t window) {
    if (SEQ_LT(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_nxt)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data  = acked < t->tx_len ? acked : t->tx_len;

        t->tx_start = (t->tx_start + data) % TCP_BUF;
        t->tx_len  -= data;
        t->snd_una  = ack;
        t->retries  = 0;
        t->rto_ms   = TCP_RTO_MS;
        t->rto_at   = 0;
        if (t->snd_una != t->snd_nxt)
            tcp_arm_rto(t);

        // our FIN is the last thing there is to ack
        if (t->fin_sent && ack == t->snd_nxt) {
            if (t->state == TCP_FIN_WAIT_1)
                t->state = TCP_FIN_WAIT_2;
            else if (t->state == TCP_CLOSING) {
                t->state   = TCP_TIME_WAIT;
                t->done_at = net_now_ms() + TCP_TIMEWAIT_MS;
            } else if (t->state == TCP_LAST_ACK) {
                tcp_set_closed(t, 0);
            }
        }
        if (t->wait)
            sched_wake_queue(t->wait);
    }

    if (SEQ_LEQ(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_nxt))
        t->snd_wnd = window;
}

// out of order data just gets acked so the sender tries again
static void tcp_got_data(tcb_t *t, uint32_t seq, const uint8_t *data, size_t len, bool fin) {
    if (SEQ_LT(seq, t->rcv_nxt)) {
        uint32_t skip = t->rcv_nxt - seq;
        if (skip > len) {
            len = 0;
            fin = false;
        } else {
            data += skip;
            len  -= skip;
        }
        seq = t->rcv_nxt;
    }
    if (seq != t->rcv_nxt || t->peer_fin)
        return;

    uint32_t room = TCP_BUF - t->rx_len;
    uint32_t take = len < room ? (uint32_t)len : room;
    uint32_t at   = (t->rx_start + t->rx_len) % TCP_BUF;
    uint32_t first = TCP_BUF - at < take ? TCP_BUF - at : take;

    memcpy(t->rx + at, data, first);
    memcpy(t->rx, data + first, take - first);
    t->rx_len  += take;
    t->rcv_nxt += take;

    if (fin && take == len) {
        t->rcv_nxt++;
        t->peer_fin = true;
        if (t->state == TCP_ESTABLISHED) {
            t->state = TCP_CLOSE_WAIT;
        } else if (t->state == TCP_FIN_WAIT_1) {
            t->state = TCP_CLOSING;
        } else if (t->state == TCP_FIN_WAIT_2) {
            t->state   = TCP_TIME_WAIT;
            t->done_at = net_now_ms() + TCP_TIMEWAIT_MS;
        }
    }

    if ((take || t->peer_fin) && t->wait)
        sched_wake_queue(t->wait);
}

void tcp_input(netif_t *nif, uint32_t src, uint32_t dst, const void *data, size_t len) {
    (void)nif;
    if (len < TCP_HLEN)
        return;

    const tcp_hdr_t *h    = data;
    size_t           hlen = (size_t)(h->off >> 4) * 4;
    if (hlen < TCP_HLEN || hlen > len)
        return;
    if (net_checksum(data, len, ipv4_pseudo_sum(src, dst, IP_PROTO_TCP, len)) != 0)
        return;

    uint32_t       seq   = ntohl(h->seq);
    uint32_t       ack   = ntohl(h->ack);
    const uint8_t *body  = (const uint8_t *)data + hlen;
    size_t         blen  = len - hlen;
    tcp_out_t      out   = { .count = 0 };

    // irqs off till the wakes are done, a closing socket frees its queue
    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    tcb_t *t = tcp_find(src, ntohs(h->src_port), dst, ntohs(h->dst_port));
    if (!t || t->state == TCP_CLOSED) {
        // nothing listens yet, so every stray segment gets told so
        if (!(h->flags & TCP_RST))
            tcp_reset_reply(dst, src, h, blen, &out);
    } else if (t->state == TCP_SYN_SENT) {
        tcp_syn_sent(t, h, hlen, seq, ack, &out);
    } else if (h->flags & TCP_RST) {
        uint32_t win = tcp_rcv_window(t);
        if (seq == t->rcv_nxt || (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + (win ? win : 1)))) {
            serial_printf(LOG_INFO "tcp: " IP_FMT ":%u reset the connection\n", IP_ARGS(t->remote_ip), t->remote_port);
            tcp_set_closed(t, t->peer_fin ? 0 : -ECONNRESET);
        }
    } else {
        if (h->flags & TCP_ACK)
            tcp_got_ack(t, ack, ntohs(h->window));

        bool fin  = (h->flags & TCP_FIN) != 0;
        bool data = blen || fin;
        if (data && (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2))
            tcp_got_data(t, seq, body, blen, fin);

        if (t->state != TCP_CLOSED) {
            int before = out.count;
            tcp_push(t, &out);
            // ack everything, even duplicates
            if (data && out.count == before)
                tcp_send_ctl(t, TCP_ACK, t->snd_nxt, &out);
        }
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
}
