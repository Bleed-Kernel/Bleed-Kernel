#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500

#define NETIF_MAX       4
#define NETBUF_SIZE     2048
#define NETBUF_HEADROOM 64

#define NET_SERVICE_MS  100

// a ping somewhere out on the internet says whether we can get past the gateway
#define NET_PROBE_IP        IP4(1, 1, 1, 1)
#define NET_PROBE_MS        30000   // between checks while it answers
#define NET_PROBE_RETRY_MS  5000
#define NET_PROBE_WAIT_MS   3000    // how long an answer gets
#define NET_PROBE_MISSES    2       // unanswered in a row before we call it offline

// everything on the wire is big endian
#define htons(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define ntohs(x) htons(x)
#define htonl(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define ntohl(x) htonl(x)

// addresses are kept in network order
#define IP4(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define IP_FMT          "%u.%u.%u.%u"
#define IP_ARGS(ip)     (unsigned)((ip) & 0xFF), (unsigned)(((ip) >> 8) & 0xFF), \
                        (unsigned)(((ip) >> 16) & 0xFF), (unsigned)(((ip) >> 24) & 0xFF)
#define MAC_FMT         "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARGS(m)     (m)[0], (m)[1], (m)[2], (m)[3], (m)[4], (m)[5]

typedef struct netbuf {
    uint8_t *data;
    size_t   len;
    uint8_t  buf[NETBUF_SIZE];
} netbuf_t;

typedef struct netif netif_t;

typedef struct netif_ops {
    int  (*send)(netif_t *nif, const void *frame, size_t len);
    void (*receive)(netif_t *nif);      // hands every waiting frame to ether_input
    bool (*link_up)(netif_t *nif);
} netif_ops_t;

struct netif {
    char               name[8];
    uint8_t            mac[ETH_ALEN];
    uint32_t           ip;          // 0 until dhcp hands one out
    uint32_t           netmask;
    uint32_t           gateway;
    uint32_t           dns;
    bool               configured;
    bool               link;
    bool               online;
    bool               probed;
    uint64_t           probe_sent;  // 0 when nothing is waiting on an answer
    uint64_t           probe_at;
    uint32_t           probe_rtt;
    uint16_t           probe_seq;
    uint8_t            probe_misses;
    uint64_t           rx_frames;
    uint64_t           tx_frames;
    uint64_t           rx_bytes;
    uint64_t           tx_bytes;
    const netif_ops_t *ops;
    void              *driver;
};

// a buffer with its data pointer NETBUF_HEADROOM in
netbuf_t *netbuf_alloc(void);
void      netbuf_free(netbuf_t *nb);

void *netbuf_push(netbuf_t *nb, size_t len);
void *netbuf_put(netbuf_t *nb, size_t len);

// add an interface, we just go null when theres no room
netif_t *netif_register(const uint8_t mac[ETH_ALEN], const netif_ops_t *ops, void *driver);

// the first interface
netif_t *netif_default(void);

// null past end
netif_t *netif_get(int index);

// an echo reply came back for the internet probe
void net_probe_answered(netif_t *nif);

// internet checksum, start is a previous sum or 0
uint16_t net_checksum(const void *data, size_t len, uint32_t start);

// sum without folding, for the udp pseudo header
uint32_t net_checksum_partial(const void *data, size_t len, uint32_t start);

uint64_t net_now_ms(void);
void net_kick(void);
void net_service(void);
