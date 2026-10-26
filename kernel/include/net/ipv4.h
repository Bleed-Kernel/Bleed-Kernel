#pragma once

#include <net/net.h>

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

#define IP_HLEN         20
#define IP_TTL          64
#define IP_FLAG_MF      0x2000
#define IP_FRAG_MASK    0x1FFF
#define IP_BROADCAST    0xFFFFFFFFu
#define IP_MAX_PAYLOAD  (0xFFFF - IP_HLEN)
#define IP_FRAG_CHUNK   (ETH_MTU - IP_HLEN)     // 1480, offsets are in 8 byte units

#define IP_FRAG_SLOTS       4       // packets being put back together at once
#define IP_FRAG_TIMEOUT_MS  5000

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ipv4_hdr_t;

void ipv4_input(netif_t *nif, const void *data, size_t len);
void ipv4_deliver(netif_t *nif, uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len);

// any size up to IP_MAX_PAYLOAD, fragmented when it doesnt fit
int ipv4_output_flat(netif_t *nif, const void *payload, size_t len, uint32_t src, uint32_t dst, uint8_t proto);

// one fragment in, the whole payload out (kmalloced, the caller frees it)
uint8_t *ipv4_reassemble(const ipv4_hdr_t *ip, const uint8_t *payload, size_t len, size_t *out_len);

// drops half built packets nobody finished
void ipv4_frag_tick(void);

// pseudo header sum for udp and tcp checksums
uint32_t ipv4_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, size_t len);
int ipv4_output(netif_t *nif, netbuf_t *nb, uint32_t src, uint32_t dst, uint8_t proto);

// returns null when not configured
netif_t *ipv4_route(uint32_t dst);
uint32_t ipv4_source(netif_t *nif, uint32_t dst);

#define IP_LOOPBACK(ip) (((ip) & 0xFF) == 127)
