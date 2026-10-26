#pragma once

#include <net/net.h>

#define UDP_HLEN        8
#define UDP_PORTS_MAX   16
#define UDP_MAX_PAYLOAD (0xFFFF - 20 - UDP_HLEN)
#define UDP_FRAME_MAX   (ETH_MTU - 20 - UDP_HLEN)  // the most that goes out without fragmenting

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} udp_hdr_t;

// data is only good till the handler returns, ports are in host order
typedef void (*udp_handler_t)(netif_t *nif, uint32_t src, uint16_t src_port, uint32_t dst,
                              const void *data, size_t len);

void udp_input(netif_t *nif, uint32_t src, uint32_t dst, const void *data, size_t len);

// nb holds just the payload, the udp header goes on in front.
int udp_output(netif_t *nif, netbuf_t *nb, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port);

// a datagram too big for a netbuf, it goes out in ip fragments
int udp_output_flat(netif_t *nif, const void *data, size_t len, uint32_t src, uint16_t src_port,
                    uint32_t dst, uint16_t dst_port);

// a port the kernel answers itself, -EADDRINUSE if someone has it
int udp_listen(uint16_t port, udp_handler_t handler);
bool udp_port_listening(uint16_t port);
