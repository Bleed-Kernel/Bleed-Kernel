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
#define TCP_OPT_WSCALE  3
#define TCP_OPT_SACK_OK 4
#define TCP_OPT_SACK    5

#define TCP_SYN_OPTS    12      // mss, window scale, sack ok with their nops
#define TCP_SACK_OPTS   28      // room for three blocks

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
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
} tcp_state_t;

typedef enum {
    TCP_RECOVERY_NONE,
    TCP_RECOVERY_FAST,          // dup acks, one hole at a time
    TCP_RECOVERY_RTO,           // timed out, everything from snd_una goes again
} tcp_recovery_t;

// bytes already sitting in the rx ring past a hole
typedef struct {
    uint32_t start;
    uint32_t end;
} tcp_range_t;

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
    uint32_t      snd_wl1;      // seq of the segment that last set snd_wnd
    uint16_t      mss;
    uint8_t       snd_shift;    // window scale, 0 when the peer didnt offer it
    uint8_t       rcv_shift;
    uint8_t       shift_next;   // rcv_shift once the handshake lands, the syn ack goes out unscaled
    bool          sack_ok;
    uint32_t      rcv_nxt;
    uint32_t      rcv_adv;      // right edge we last told the peer about
    tcp_range_t   ooo[TCP_OOO_MAX];
    int           ooo_count;
    uint32_t      fin_seq;
    bool          fin_seen;     // came in order or not, waits for rcv_nxt

    uint32_t      cwnd;
    uint32_t      ssthresh;
    int           dupacks;
    tcp_recovery_t recovery;
    uint32_t      recover;      // recovery ends once this is acked
    uint32_t      rtx_nxt;
    bool          rtx_one;      // fast recovery owes one retransmit

    uint64_t      rtt_at;       // when rtt_seq left, 0 when nothing is being timed
    uint32_t      rtt_seq;
    uint32_t      srtt;
    uint32_t      rttvar;
    uint32_t      rto_base;     // what rto_ms goes back to after a backoff

    uint8_t      *rx;           // rings, start is the oldest byte
    uint32_t      rx_start;
    uint32_t      rx_len;
    uint8_t      *tx;
    uint32_t      tx_start;
    uint32_t      tx_len;       // acked bytes leave, so this is in flight plus not sent yet

    bool          fin_wanted;   // close once tx is out
    bool          fin_sent;
    bool          peer_fin;
    bool          rd_shut;
    bool          was_connected;
    int           error;        // negative errno for SO_ERROR and the next call

    uint64_t      rto_at;       // 0 when nothing is waiting on an ack
    uint32_t      rto_ms;
    int           retries;
    uint64_t      done_at;      // time wait and linger both end here

    wait_queue_t *wait;         // the sockets, NULL once its gone
    bool          detached;

    bool          keepalive;
    bool          nodelay;
    uint32_t      keep_idle;    // seconds
    uint32_t      keep_intvl;
    uint32_t      keep_cnt;
    uint32_t      keep_sent;
    uint64_t      last_rx;

    struct tcb   *parent;       // the listener, till accept takes it
    struct tcb   *accept_head;
    struct tcb   *accept_tail;
    struct tcb   *accept_next;
    int           backlog;
    int           pending;      // half open plus waiting in the queue
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
void     tcp_send_ack(tcb_t *t, tcp_out_t *out);
void     tcp_send_syn(tcb_t *t, tcp_out_t *out);
void     tcp_push(tcb_t *t, tcp_out_t *out);
void     tcp_reset_reply(uint32_t src, uint32_t dst, const tcp_hdr_t *in, size_t dlen, tcp_out_t *out);
void     tcp_set_closed(tcb_t *t, int err);
void     tcp_arm_rto(tcb_t *t);
uint16_t tcp_rcv_window(const tcb_t *t);
void     tcp_unlink(tcb_t *t);
void     tcp_free(tcb_t *t);
tcb_t   *tcp_alloc(void);
int      tcp_alloc_rings(tcb_t *t);
uint32_t tcp_new_iss(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);
void     tcp_drop_child(tcb_t *t);
