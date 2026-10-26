#pragma once

#include <net/net.h>

#define ICMP_ECHO_REPLY     0
#define ICMP_UNREACHABLE    3
#define ICMP_ECHO_REQUEST   8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

void icmp_input(netif_t *nif, uint32_t src, const void *data, size_t len);

// send one echo request, the reply only gets logged for now
int icmp_ping(uint32_t dst, uint16_t seq);
