#include "tcp_priv.h"
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

typedef struct {
    uint16_t mss;
    uint8_t  wscale;
    bool     has_wscale;
    bool     sack_ok;
} tcp_syn_opts_t;

// a real connection first, then whatever listens on the port
static tcb_t *tcp_find(uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port) {
    tcb_t *listener = NULL;

    for (tcb_t *t = tcp_list; t; t = t->next) {
        if (t->local_port != dst_port || t->state == TCP_CLOSED)
            continue;
        if (t->local_ip && t->local_ip != dst)
            continue;
        if (t->state == TCP_LISTEN)
            listener = t;
        else if (t->remote_port == src_port && t->remote_ip == src)
            return t;
    }
    return listener;
}

static void tcp_parse_syn(const tcp_hdr_t *h, size_t hlen, tcp_syn_opts_t *o) {
    const uint8_t *p   = (const uint8_t *)h + TCP_HLEN;
    const uint8_t *end = (const uint8_t *)h + hlen;

    o->mss        = TCP_MSS_DEFAULT;
    o->wscale     = 0;
    o->has_wscale = false;
    o->sack_ok    = false;

    while (p < end && *p != TCP_OPT_END) {
        if (*p == TCP_OPT_NOP) {
            p++;
            continue;
        }
        if (p + 2 > end || p[1] < 2 || p + p[1] > end)
            break;
        if (p[0] == TCP_OPT_MSS && p[1] == 4)
            o->mss = (uint16_t)(p[2] << 8 | p[3]);
        else if (p[0] == TCP_OPT_WSCALE && p[1] == 3) {
            o->has_wscale = true;
            o->wscale     = p[2] > 14 ? 14 : p[2];
        } else if (p[0] == TCP_OPT_SACK_OK && p[1] == 2)
            o->sack_ok = true;
        p += p[1];
    }
}

static void tcp_use_syn_opts(tcb_t *t, const tcp_syn_opts_t *o) {
    t->mss       = o->mss < TCP_MSS ? o->mss : TCP_MSS;
    t->snd_shift = o->has_wscale ? o->wscale : 0;
    t->rcv_shift = o->has_wscale ? TCP_WSCALE : 0;
    t->sack_ok   = o->sack_ok;
    t->cwnd      = TCP_INIT_CWND * (uint32_t)t->mss;
    t->ssthresh  = 0xFFFFFFFFu;
    t->last_rx   = net_now_ms();
}

static void tcp_rtt_sample(tcb_t *t) {
    uint32_t rtt = (uint32_t)(net_now_ms() - t->rtt_at);
    t->rtt_at = 0;

    if (!t->srtt) {
        t->srtt   = rtt ? rtt : 1;
        t->rttvar = rtt / 2;
    } else {
        uint32_t delta = rtt > t->srtt ? rtt - t->srtt : t->srtt - rtt;
        t->rttvar = (3 * t->rttvar + delta) / 4;
        t->srtt   = (7 * t->srtt + rtt) / 8;
    }

    uint32_t rto = t->srtt + 4 * t->rttvar;
    if (rto < TCP_RTO_MIN_MS) rto = TCP_RTO_MIN_MS;
    if (rto > TCP_RTO_MAX_MS) rto = TCP_RTO_MAX_MS;
    t->rto_base = rto;
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

    tcp_syn_opts_t o;
    tcp_parse_syn(h, hlen, &o);
    tcp_use_syn_opts(t, &o);

    t->rcv_nxt       = seq + 1;
    t->snd_una       = ack;
    t->snd_wnd       = ntohs(h->window);    // never scaled on a syn
    t->snd_wl1       = seq;
    t->state         = TCP_ESTABLISHED;
    t->was_connected = true;
    t->rto_at        = 0;
    t->rto_ms        = t->rto_base;
    t->retries       = 0;

    serial_printf(LOG_INFO "tcp: connected to " IP_FMT ":%u from port %u, mss %u\n",
                  IP_ARGS(t->remote_ip), t->remote_port, t->local_port, t->mss);
    tcp_send_ctl(t, TCP_ACK, t->snd_nxt, out);
    if (t->wait)
        sched_wake_queue(t->wait);
}

// a new connection for a listener, the socket only turns up at accept
static void tcp_listen_input(tcb_t *l, uint32_t src, uint32_t dst, const tcp_hdr_t *h, size_t hlen,
                             size_t blen, uint32_t seq, tcp_out_t *out) {
    if (h->flags & TCP_RST)
        return;
    if (h->flags & TCP_ACK) {
        tcp_reset_reply(dst, src, h, blen, out);
        return;
    }
    if (!(h->flags & TCP_SYN) || l->pending >= l->backlog)
        return;

    tcb_t *t = tcp_alloc();
    if (!t)
        return;

    tcp_syn_opts_t o;
    tcp_parse_syn(h, hlen, &o);
    tcp_use_syn_opts(t, &o);
    t->rcv_shift = 0;   // the syn ack window goes out unscaled

    t->local_ip    = dst;
    t->local_port  = ntohs(h->dst_port);
    t->remote_ip   = src;
    t->remote_port = ntohs(h->src_port);
    t->iss         = tcp_new_iss(dst, t->local_port, src, t->remote_port);
    t->snd_una     = t->iss;
    t->snd_nxt     = t->iss + 1;
    t->snd_wnd     = ntohs(h->window);
    t->snd_wl1     = seq;
    t->rcv_nxt     = seq + 1;
    t->state       = TCP_SYN_RCVD;
    t->parent      = l;
    t->keepalive   = l->keepalive;
    t->nodelay     = l->nodelay;
    t->keep_idle   = l->keep_idle;
    t->keep_intvl  = l->keep_intvl;
    t->keep_cnt    = l->keep_cnt;
    t->shift_next  = o.has_wscale ? TCP_WSCALE : 0;
    l->pending++;

    t->next   = tcp_list;
    tcp_list  = t;
    t->linked = true;

    tcp_send_syn(t, out);
    tcp_arm_rto(t);
}

// false when the connection didnt survive
static bool tcp_syn_rcvd(tcb_t *t, const tcp_hdr_t *h, uint32_t seq, uint32_t ack, tcp_out_t *out) {
    if (h->flags & TCP_RST) {
        if (seq == t->rcv_nxt)
            tcp_drop_child(t);
        return false;
    }
    if ((h->flags & TCP_SYN) && seq + 1 == t->rcv_nxt) {
        tcp_send_syn(t, out);
        return false;
    }
    if (!(h->flags & TCP_ACK))
        return false;
    if (ack != t->snd_nxt) {
        tcp_send_ctl(t, TCP_RST, ack, out);
        return false;
    }
    if (tcp_alloc_rings(t) != 0) {
        tcp_send_ctl(t, TCP_RST, ack, out);
        tcp_drop_child(t);
        return false;
    }

    tcb_t *l = t->parent;
    t->rcv_shift     = t->shift_next;
    t->snd_una       = ack;
    t->state         = TCP_ESTABLISHED;
    t->was_connected = true;
    t->rto_at        = 0;
    t->rto_ms        = t->rto_base;
    t->retries       = 0;

    if (l->accept_tail)
        l->accept_tail->accept_next = t;
    else
        l->accept_head = t;
    l->accept_tail = t;

    serial_printf(LOG_INFO "tcp: " IP_FMT ":%u connected to port %u, mss %u\n",
                  IP_ARGS(t->remote_ip), t->remote_port, t->local_port, t->mss);
    if (l->wait)
        sched_wake_queue(l->wait);
    return true;
}

static void tcp_cc_acked(tcb_t *t, uint32_t acked, uint32_t ack) {
    if (t->recovery != TCP_RECOVERY_NONE && SEQ_LT(ack, t->recover)) {
        if (t->recovery == TCP_RECOVERY_FAST) {
            // partial ack, the next hole is right here
            t->cwnd    = t->cwnd > acked ? t->cwnd - acked + t->mss : t->mss;
            t->rtx_one = true;
            return;
        }
    } else if (t->recovery != TCP_RECOVERY_NONE) {
        if (t->recovery == TCP_RECOVERY_FAST)
            t->cwnd = t->ssthresh;
        t->recovery = TCP_RECOVERY_NONE;
        t->dupacks  = 0;
        return;
    }

    t->dupacks = 0;
    if (t->cwnd < t->ssthresh) {
        t->cwnd += acked < t->mss ? acked : t->mss;
    } else {
        uint32_t inc = (uint32_t)t->mss * t->mss / t->cwnd;
        t->cwnd += inc ? inc : 1;
    }
    if (t->cwnd > 0x40000000u)
        t->cwnd = 0x40000000u;
}

static void tcp_cc_dupack(tcb_t *t) {
    if (t->recovery == TCP_RECOVERY_FAST) {
        t->cwnd += t->mss;
        return;
    }
    if (t->recovery != TCP_RECOVERY_NONE || ++t->dupacks < 3)
        return;

    uint32_t half = (t->snd_nxt - t->snd_una) / 2;
    t->ssthresh = half > 2u * t->mss ? half : 2u * t->mss;
    t->cwnd     = t->ssthresh + 3u * t->mss;
    t->recovery = TCP_RECOVERY_FAST;
    t->recover  = t->snd_nxt;
    t->rtx_one  = true;
}

static void tcp_got_ack(tcb_t *t, uint32_t seq, uint32_t ack, uint16_t raw_window, bool bare) {
    uint32_t window  = (uint32_t)raw_window << t->snd_shift;
    bool     moved   = window != t->snd_wnd;

    if (SEQ_LT(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_nxt)) {
        uint32_t acked = ack - t->snd_una;
        uint32_t data  = acked < t->tx_len ? acked : t->tx_len;

        t->tx_start = (t->tx_start + data) % TCP_TX_BUF;
        t->tx_len  -= data;
        t->snd_una  = ack;
        t->retries  = 0;
        t->rto_ms   = t->rto_base;
        t->rto_at   = 0;

        if (t->rtt_at && SEQ_LT(t->rtt_seq, ack))
            tcp_rtt_sample(t);
        t->rto_ms = t->rto_base;
        if (t->snd_una != t->snd_nxt)
            tcp_arm_rto(t);

        tcp_cc_acked(t, acked, ack);

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
    } else if (ack == t->snd_una && bare && !moved && t->snd_una != t->snd_nxt) {
        tcp_cc_dupack(t);
    }

    // the window opened, the probe byte got thrown away so its just unsent again
    if (!t->snd_wnd && window && ack == t->snd_una && t->snd_nxt - ack == 1 && !t->fin_sent) {
        t->snd_nxt = ack;
        t->rto_at  = 0;
        t->rto_ms  = t->rto_base;
    }

    // a shut peer still answering keeps the persist timer from giving up
    if (!window && ack == t->snd_una)
        t->retries = 0;

    // old segments dont get to shrink the window back
    if (SEQ_LEQ(t->snd_una, ack) && SEQ_LEQ(ack, t->snd_nxt) && SEQ_LEQ(t->snd_wl1, seq)) {
        t->snd_wnd = window;
        t->snd_wl1 = seq;
    }
}

static void tcp_ooo_add(tcb_t *t, uint32_t start, uint32_t end) {
    int i = 0;
    while (i < t->ooo_count && SEQ_LT(t->ooo[i].end, start))
        i++;

    if (i < t->ooo_count && SEQ_LEQ(t->ooo[i].start, end)) {
        if (SEQ_LT(start, t->ooo[i].start))
            t->ooo[i].start = start;
        if (SEQ_LT(t->ooo[i].end, end))
            t->ooo[i].end = end;

        // that may have closed the gap to the ones after it
        int j = i + 1;
        while (j < t->ooo_count && SEQ_LEQ(t->ooo[j].start, t->ooo[i].end)) {
            if (SEQ_LT(t->ooo[i].end, t->ooo[j].end))
                t->ooo[i].end = t->ooo[j].end;
            j++;
        }
        memmove(&t->ooo[i + 1], &t->ooo[j], (size_t)(t->ooo_count - j) * sizeof(tcp_range_t));
        t->ooo_count -= j - i - 1;
        return;
    }

    if (t->ooo_count == TCP_OOO_MAX)
        return;
    memmove(&t->ooo[i + 1], &t->ooo[i], (size_t)(t->ooo_count - i) * sizeof(tcp_range_t));
    t->ooo[i].start = start;
    t->ooo[i].end   = end;
    t->ooo_count++;
}

// the hole in front of the first range is filled, those bytes are readable now
static void tcp_ooo_settle(tcb_t *t) {
    while (t->ooo_count && SEQ_LEQ(t->ooo[0].start, t->rcv_nxt)) {
        if (SEQ_LT(t->rcv_nxt, t->ooo[0].end)) {
            t->rx_len += t->ooo[0].end - t->rcv_nxt;
            t->rcv_nxt = t->ooo[0].end;
        }
        t->ooo_count--;
        memmove(&t->ooo[0], &t->ooo[1], (size_t)t->ooo_count * sizeof(tcp_range_t));
    }
}

// bytes go straight to their spot in the ring, a hole is just bytes nobody counted yet
static void tcp_got_data(tcb_t *t, uint32_t seq, const uint8_t *data, size_t len, bool fin) {
    if (SEQ_LT(seq, t->rcv_nxt)) {
        uint32_t skip = t->rcv_nxt - seq;
        if (skip > len)
            return;
        data += skip;
        len  -= skip;
        seq   = t->rcv_nxt;
    }
    if (t->peer_fin)
        return;

    uint32_t off  = seq - t->rcv_nxt;
    uint32_t room = TCP_RX_BUF - t->rx_len;
    if (off && off >= room)
        return;

    uint32_t take  = len < room - off ? (uint32_t)len : room - off;
    uint32_t at    = (t->rx_start + t->rx_len + off) % TCP_RX_BUF;
    uint32_t first = TCP_RX_BUF - at < take ? TCP_RX_BUF - at : take;

    memcpy(t->rx + at, data, first);
    memcpy(t->rx, data + first, take - first);

    uint32_t before = t->rx_len;
    if (fin && take == len) {
        t->fin_seen = true;
        t->fin_seq  = seq + take;
    }
    if (off) {
        if (take)
            tcp_ooo_add(t, seq, seq + take);
    } else {
        t->rx_len  += take;
        t->rcv_nxt += take;
        tcp_ooo_settle(t);
    }

    if (t->fin_seen && t->fin_seq == t->rcv_nxt) {
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

    if ((t->rx_len != before || t->peer_fin) && t->wait)
        sched_wake_queue(t->wait);
}

static bool tcp_can_take_data(const tcb_t *t) {
    return t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 || t->state == TCP_FIN_WAIT_2;
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

    // the peer came back on the same ports, the old connection is done with
    if (t && t->state == TCP_TIME_WAIT && (h->flags & TCP_SYN) && !(h->flags & TCP_ACK) && SEQ_LT(t->rcv_nxt, seq)) {
        tcp_set_closed(t, 0);
        t = tcp_find(src, ntohs(h->src_port), dst, ntohs(h->dst_port));
    }

    if (!t) {
        if (!(h->flags & TCP_RST))
            tcp_reset_reply(dst, src, h, blen, &out);
    } else if (t->state == TCP_LISTEN) {
        tcp_listen_input(t, src, dst, h, hlen, blen, seq, &out);
    } else if (t->state == TCP_SYN_SENT) {
        tcp_syn_sent(t, h, hlen, seq, ack, &out);
    } else if (h->flags & TCP_RST) {
        if (t->state == TCP_SYN_RCVD) {
            tcp_syn_rcvd(t, h, seq, ack, &out);
        } else {
            uint32_t win = TCP_RX_BUF - t->rx_len;
            if (seq == t->rcv_nxt || (SEQ_LEQ(t->rcv_nxt, seq) && SEQ_LT(seq, t->rcv_nxt + (win ? win : 1)))) {
                serial_printf(LOG_INFO "tcp: " IP_FMT ":%u reset the connection\n", IP_ARGS(t->remote_ip), t->remote_port);
                tcp_set_closed(t, t->peer_fin ? 0 : -ECONNRESET);
            }
        }
    } else if (t->state != TCP_SYN_RCVD || tcp_syn_rcvd(t, h, seq, ack, &out)) {
        t->last_rx   = net_now_ms();
        t->keep_sent = 0;

        bool fin    = (h->flags & TCP_FIN) != 0;
        bool carry  = blen || fin;
        bool answer = carry || (h->flags & TCP_SYN);    // a repeated syn ack means our ack got lost

        if ((h->flags & TCP_ACK) && SEQ_LT(t->snd_nxt, ack)) {
            // acks something we never sent
            tcp_send_ctl(t, TCP_ACK, t->snd_nxt, &out);
        } else if (t->detached && blen && SEQ_LT(t->rcv_nxt, seq + (uint32_t)blen) && tcp_can_take_data(t)) {
            // nobody left to read it
            tcp_send_ctl(t, TCP_RST | TCP_ACK, ack, &out);
            tcp_set_closed(t, 0);
        } else {
            if (h->flags & TCP_ACK)
                tcp_got_ack(t, seq, ack, ntohs(h->window), !carry);

            if (carry && tcp_can_take_data(t))
                tcp_got_data(t, seq, body, blen, fin);

            if (t->state != TCP_CLOSED) {
                tcp_push(t, &out);
                // ack everything, even duplicates, thats what the peer counts for fast retransmit
                if (answer)
                    tcp_send_ack(t, &out);
            }
        }
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
    tcp_out_flush(&out);
}

void tcp_unreachable(uint8_t code, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, uint32_t seq) {
    unsigned long flags = irq_push();
    spinlock_acquire(&tcp_lock);

    tcb_t *t = tcp_find(dst, dst_port, src, src_port);
    if (t && t->state == TCP_SYN_SENT && seq == t->iss) {
        int err = -EHOSTUNREACH;
        if (code == 0)
            err = -ENETUNREACH;
        else if (code == 2 || code == 3)
            err = -ECONNREFUSED;
        tcp_set_closed(t, err);
    }

    spinlock_release(&tcp_lock);
    irq_restore(flags);
}
