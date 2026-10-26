#pragma once

#include <stdint.h>

#define NET_DEVICE_INFO     0x4E370001
#define NET_TEXT_MAX        512

#define NET_STATE_NONE          0
#define NET_STATE_UNPLUGGED     1
#define NET_STATE_CONNECTING    2
#define NET_STATE_NO_INTERNET   3
#define NET_STATE_ONLINE        4

// what NET_DEVICE_INFO hands back, addresses are in network order
typedef struct net_device_info {
    uint8_t  state;
    uint8_t  mac[6];
    uint8_t  reserved;
    char     name[8];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint32_t rtt_ms;
    uint32_t reserved2;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
} net_device_info_t;

void net_device_init(void);
