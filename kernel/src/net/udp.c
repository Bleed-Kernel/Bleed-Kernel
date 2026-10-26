#include <net/udp.h>
#include <net/ipv4.h>
#include <net/socket.h>
#include <drivers/serial/serial.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

typedef struct {
    uint16_t      port;
    udp_handler_t handler;
} udp_port_t;

static udp_port_t udp_ports[UDP_PORTS_MAX];
static spinlock_t udp_lock;

static uint16_t udp_checksum(uint32_t src, uint32_t dst, const void *udp, size_t len) {
    return net_checksum(udp, len, ipv4_pseudo_sum(src, dst, IP_PROTO_UDP, len));
}

static void udp_header(udp_hdr_t *udp, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, size_t len) {
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->len      = htons((uint16_t)len);
    udp->checksum = 0;
    udp->checksum = udp_checksum(src, dst, udp, len);
    if (!udp->checksum)
        udp->checksum = 0xFFFF;     // 0 would read as no checksum
}

void udp_input(netif_t *nif, uint32_t src, uint32_t dst, const void *data, size_t len) {
    if (len < UDP_HLEN)
        return;

    const udp_hdr_t *udp  = data;
    size_t           ulen = ntohs(udp->len);
    if (ulen < UDP_HLEN || ulen > len)
        return;

    // 0 means the sender didnt bother
    if (udp->checksum && udp_checksum(src, dst, udp, ulen) != 0)
        return;

    uint16_t      port    = ntohs(udp->dst_port);
    udp_handler_t handler = NULL;

    unsigned long flags = irq_push();
    spinlock_acquire(&udp_lock);
    for (int i = 0; i < UDP_PORTS_MAX; i++) {
        if (udp_ports[i].handler && udp_ports[i].port == port) {
            handler = udp_ports[i].handler;
            break;
        }
    }
    spinlock_release(&udp_lock);
    irq_restore(flags);

    if (handler)
        handler(nif, src, ntohs(udp->src_port), dst, udp + 1, ulen - UDP_HLEN);
    else
        socket_udp_input(src, ntohs(udp->src_port), dst, port, udp + 1, ulen - UDP_HLEN);
}

bool udp_port_listening(uint16_t port) {
    bool found = false;

    unsigned long flags = irq_push();
    spinlock_acquire(&udp_lock);
    for (int i = 0; i < UDP_PORTS_MAX && !found; i++)
        found = udp_ports[i].handler && udp_ports[i].port == port;
    spinlock_release(&udp_lock);
    irq_restore(flags);
    return found;
}

int udp_output(netif_t *nif, netbuf_t *nb, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port) {
    if (!nif)
        nif = ipv4_route(dst);
    if (!nif) {
        netbuf_free(nb);
        return -ENETUNREACH;
    }

    udp_hdr_t *udp = netbuf_push(nb, UDP_HLEN);
    if (!udp) {
        netbuf_free(nb);
        return -ENOBUFS;
    }

    if (!src)
        src = ipv4_source(nif, dst);

    udp_header(udp, src, src_port, dst, dst_port, nb->len);
    return ipv4_output(nif, nb, src, dst, IP_PROTO_UDP);
}

int udp_output_flat(netif_t *nif, const void *data, size_t len, uint32_t src, uint16_t src_port,
                    uint32_t dst, uint16_t dst_port) {
    if (!nif)
        nif = ipv4_route(dst);
    if (!nif)
        return -ENETUNREACH;
    if (len > UDP_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (!src)
        src = ipv4_source(nif, dst);

    uint8_t *buf = kmalloc(UDP_HLEN + len);
    if (!buf)
        return -ENOMEM;
    memcpy(buf + UDP_HLEN, data, len);
    udp_header((udp_hdr_t *)buf, src, src_port, dst, dst_port, UDP_HLEN + len);

    int err = ipv4_output_flat(nif, buf, UDP_HLEN + len, src, dst, IP_PROTO_UDP);
    kfree(buf);
    return err;
}

int udp_listen(uint16_t port, udp_handler_t handler) {
    int err = -ENOBUFS;

    unsigned long flags = irq_push();
    spinlock_acquire(&udp_lock);
    for (int i = 0; i < UDP_PORTS_MAX; i++) {
        if (udp_ports[i].handler && udp_ports[i].port == port) {
            err = -EADDRINUSE;
            break;
        }
    }
    if (err != -EADDRINUSE) {
        for (int i = 0; i < UDP_PORTS_MAX; i++) {
            if (!udp_ports[i].handler) {
                udp_ports[i].port    = port;
                udp_ports[i].handler = handler;
                err = 0;
                break;
            }
        }
    }
    spinlock_release(&udp_lock);
    irq_restore(flags);
    return err;
}
