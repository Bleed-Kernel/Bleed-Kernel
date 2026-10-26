#pragma once

#include <net/net.h>

#define ARP_HW_ETHER    1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

#define ARP_CACHE_SIZE  32
#define ARP_RETRY_MS    1000
#define ARP_TRIES       3
#define ARP_TTL_MS      (10 * 60 * 1000)

typedef struct __attribute__((packed)) {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t op;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} arp_pkt_t;

void arp_input(netif_t *nif, const void *data, size_t len);

// send to next_hop, held while the mac is looked up. nb is always taken
int arp_output(netif_t *nif, netbuf_t *nb, uint32_t next_hop);

// retries and expiry, off the net service
void arp_tick(void);
