#pragma once

#include <net/net.h>
#include <sched/wait_queue.h>

#define TCP_RX_BUF          262144
#define TCP_TX_BUF          131072
#define TCP_WSCALE          3       // TCP_RX_BUF >> this has to fit in 16 bits
#define TCP_MSS             1460
#define TCP_MSS_DEFAULT     536     // what the other end gets till it says otherwise
#define TCP_OOO_MAX         8       // holes tracked, more get dropped and resent
#define TCP_INIT_CWND       10      // segments
#define TCP_RTO_MS          1000
#define TCP_RTO_MIN_MS      300     // the service only ticks every 100ms
#define TCP_RTO_MAX_MS      30000
#define TCP_SYN_RETRIES     5
#define TCP_RETRIES         8
#define TCP_TIMEWAIT_MS     2000
#define TCP_LINGER_MS       30000   // before a closed socket gets reset
#define TCP_BACKLOG_MAX     128
#define TCP_KEEP_IDLE_S     7200
#define TCP_KEEP_INTVL_S    75
#define TCP_KEEP_CNT        9

// setsockopt names at the IPPROTO_TCP level
#define TCP_NODELAY         1
#define TCP_MAXSEG          2
#define TCP_KEEPIDLE        4
#define TCP_KEEPINTVL       5
#define TCP_KEEPCNT         6

typedef struct tcb tcb_t;

// a connection for a socket, wait gets woken on any change
tcb_t *tcp_create(wait_queue_t *wait);

// send the SYN, ports in host order. the result turns up in tcp_connect_result
int tcp_connect(tcb_t *t, uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);

// start taking connections on a bound port
int tcp_listen(tcb_t *t, uint32_t local_ip, uint16_t local_port, int backlog);

// the oldest finished connection, -EAGAIN when there isnt one. wait is its new sockets queue
int tcp_accept(tcb_t *t, wait_queue_t *wait, tcb_t **child);

// where a connection really goes, ports in host order
void tcp_endpoints(tcb_t *t, uint32_t *local_ip, uint16_t *local_port, uint32_t *remote_ip, uint16_t *remote_port);

// half close, either side
int tcp_shutdown(tcb_t *t, bool rd, bool wr);

// -EINPROGRESS while the handshake is out, 0 once its through, or what went wrong
int tcp_connect_result(tcb_t *t);

// never block, -EAGAIN when theres no room or nothing to read. recv gives 0 at eof
long tcp_send(tcb_t *t, const void *buf, size_t len);
long tcp_recv(tcb_t *t, void *buf, size_t len, bool peek);

// POLL* bits
int tcp_poll(tcb_t *t);

// the error SO_ERROR reports, cleared by reading it
int tcp_take_error(tcb_t *t);

// bytes waiting to be read
size_t tcp_pending(tcb_t *t);

// IPPROTO_TCP options and SO_KEEPALIVE
int tcp_setopt(tcb_t *t, int name, int val);
int tcp_getopt(tcb_t *t, int name, int *val);
void tcp_set_keepalive(tcb_t *t, bool on);
bool tcp_get_keepalive(tcb_t *t);
bool tcp_is_listening(tcb_t *t);

// the socket let go, the connection closes down on its own and frees itself. abort resets it instead
void tcp_detach(tcb_t *t, bool abort);

// an icmp unreachable about a segment we sent, seq is the one quoted back
void tcp_unreachable(uint8_t code, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, uint32_t seq);
void tcp_input(netif_t *nif, uint32_t src, uint32_t dst, const void *data, size_t len);

// retransmits and timeouts, off the net service
void tcp_tick(void);
