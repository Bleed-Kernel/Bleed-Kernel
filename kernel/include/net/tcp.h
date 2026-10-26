#pragma once

#include <net/net.h>
#include <sched/wait_queue.h>

#define TCP_BUF             65535   // each way, no window scaling
#define TCP_MSS             1460
#define TCP_MSS_DEFAULT     536     // what the other end gets till it says otherwise
#define TCP_RTO_MS          1000
#define TCP_RTO_MAX_MS      8000
#define TCP_SYN_RETRIES     5
#define TCP_RETRIES         8
#define TCP_TIMEWAIT_MS     2000    // short, nothing listens anyway
#define TCP_LINGER_MS       30000   // before a closed socket gets reset

typedef struct tcb tcb_t;

// a connection for a socket, wait gets woken on any change
tcb_t *tcp_create(wait_queue_t *wait);

// send the SYN, ports in host order. the result turns up in tcp_connect_result
int tcp_connect(tcb_t *t, uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port);

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

// the socket let go, the connection closes down on its own and frees itself
void tcp_detach(tcb_t *t);
void tcp_input(netif_t *nif, uint32_t src, uint32_t dst, const void *data, size_t len);

// retransmits and timeouts, off the net service
void tcp_tick(void);
