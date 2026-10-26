#pragma once

#include <net/tcp.h>
#include <mm/spinlock.h>

#define TCP_HLEN        20
#define TCP_OUT_MAX     24      // per call, the rest go out as acks come in

#define TCP_FIN         0x01
#define TCP_SYN         0x02
#define TCP_RST         0x04
#define TCP_PSH         0x08
#define TCP_ACK         0x10

#define TCP_OPT_END     0
#define TCP_OPT_NOP     1
#define TCP_OPT_MSS     2

#define SEQ_LT(a, b)    ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)   ((int32_t)((a) - (b)) <= 0)

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off;       // header length in words, top four bits
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

typedef enum {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
} tcp_state_t;

struct tcb {
    struct tcb   *next;
    tcp_state_t   state;
    uint32_t      local_ip;
    uint32_t      remote_ip;
    uint16_t      local_port;
    uint16_t      remote_port;
    bool          linked;       // on the list tcp_input searches

    uint32_t      iss;
    uint32_t      snd_una;      // oldest byte not acked yet, the first in tx
    uint32_t      snd_nxt;
    uint32_t      snd_wnd;
    uint16_t      mss;
    uint32_t      rcv_nxt;

    uint8_t      *rx;           // rings, start is the oldest byte
    uint32_t      rx_start;
    uint32_t      rx_len;
    uint8_t      *tx;
    uint32_t      tx_start;
    uint32_t      tx_len;       // acked bytes leave, so this is in flight plus not sent yet

    bool          fin_wanted;   // close once tx is out
    bool          fin_sent;
    bool          peer_fin;
    bool          was_connected;
    int           error;        // negative errno for SO_ERROR and the next call

    uint64_t      rto_at;       // 0 when nothing is waiting on an ack
    uint32_t      rto_ms;
    int           retries;
    uint64_t      done_at;      // time wait and linger both end here

    wait_queue_t *wait;         // the sockets, NULL once its gone
    bool          detached;
};

// queued under the lock, sent after. loopback would deadlock in tcp_input
typedef struct {
    netbuf_t *nb[TCP_OUT_MAX];
    uint32_t  src[TCP_OUT_MAX];
    uint32_t  dst[TCP_OUT_MAX];
    int       count;
} tcp_out_t;

extern spinlock_t tcp_lock;
extern tcb_t     *tcp_list;

void     tcp_out_flush(tcp_out_t *out);
void     tcp_send_ctl(tcb_t *t, uint8_t flags, uint32_t seq, tcp_out_t *out);
void     tcp_send_syn(tcb_t *t, tcp_out_t *out);
void     tcp_push(tcb_t *t, tcp_out_t *out);
void     tcp_reset_reply(uint32_t src, uint32_t dst, const tcp_hdr_t *in, size_t dlen, tcp_out_t *out);
void     tcp_set_closed(tcb_t *t, int err);
void     tcp_arm_rto(tcb_t *t);
uint16_t tcp_rcv_window(const tcb_t *t);
void     tcp_unlink(tcb_t *t);
void     tcp_free(tcb_t *t);
