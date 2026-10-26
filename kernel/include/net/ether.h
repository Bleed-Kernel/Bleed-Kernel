#pragma once

#include <net/net.h>

#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} eth_hdr_t;

extern const uint8_t eth_broadcast[ETH_ALEN];

// a frame off the wire, frame is only good till this returns
void ether_input(netif_t *nif, const void *frame, size_t len);

// put the ethernet header on and send it we free nb either way
int ether_output(netif_t *nif, netbuf_t *nb, const uint8_t dst[ETH_ALEN], uint16_t type);
