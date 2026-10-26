#pragma once

#include <net/net.h>

#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68
#define DHCP_MAGIC          0x63825363
#define DHCP_RETRY_MS       2000
#define DHCP_MIN_LEASE_S    60

#define DHCP_BOOTREQUEST    1
#define DHCP_BOOTREPLY      2

#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_DECLINE        4
#define DHCP_ACK            5
#define DHCP_NAK            6

#define DHCP_OPT_PAD        0
#define DHCP_OPT_NETMASK    1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_REQ_IP     50
#define DHCP_OPT_LEASE      51
#define DHCP_OPT_MSG_TYPE   53
#define DHCP_OPT_SERVER_ID  54
#define DHCP_OPT_PARAMS     55
#define DHCP_OPT_END        255

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} dhcp_pkt_t;

// start asking for an address on nif
void dhcp_start(netif_t *nif);

// retries and renewals, off the net service
void dhcp_tick(void);
