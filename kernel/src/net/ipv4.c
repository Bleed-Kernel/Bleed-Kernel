#include <net/ipv4.h>
#include <net/arp.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

static uint16_t ip_next_id = 1;

typedef struct __attribute__((packed)) {
    uint32_t src;
    uint32_t dst;
    uint8_t  zero;
    uint8_t  proto;
    uint16_t len;
} ipv4_pseudo_t;

static bool ipv4_is_ours(netif_t *nif, uint32_t dst) {
    if (dst == IP_BROADCAST || IP_LOOPBACK(dst))
        return true;
    // still waiting on dhcp?
    if (!nif->configured)
        return true;
    if (dst == nif->ip)
        return true;
    return nif->netmask && dst == ((nif->ip & nif->netmask) | ~nif->netmask);
}

uint32_t ipv4_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, size_t len) {
    ipv4_pseudo_t ph = {
        .src   = src,
        .dst   = dst,
        .zero  = 0,
        .proto = proto,
        .len   = htons((uint16_t)len),
    };
    return net_checksum_partial(&ph, sizeof(ph), 0);
}

void ipv4_deliver(netif_t *nif, uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    switch (proto) {
        case IP_PROTO_ICMP:
            icmp_input(nif, src, payload, len);
            break;
        case IP_PROTO_UDP:
            udp_input(nif, src, dst, payload, len);
            break;
        case IP_PROTO_TCP:
            tcp_input(nif, src, dst, payload, len);
            break;
        default:
            break;
    }
}

void ipv4_input(netif_t *nif, const void *data, size_t len) {
    if (len < IP_HLEN)
        return;

    const ipv4_hdr_t *ip  = data;
    size_t            hlen = (size_t)(ip->ver_ihl & 0x0F) * 4;
    size_t            tlen = ntohs(ip->total_len);

    if ((ip->ver_ihl >> 4) != 4 || hlen < IP_HLEN || hlen > len || tlen < hlen || tlen > len)
        return;
    if (net_checksum(ip, hlen, 0) != 0)
        return;
    if (!ipv4_is_ours(nif, ip->dst))
        return;

    const uint8_t *payload = (const uint8_t *)data + hlen;
    size_t         plen    = tlen - hlen;

    if (!(ntohs(ip->frag) & (IP_FLAG_MF | IP_FRAG_MASK))) {
        ipv4_deliver(nif, ip->src, ip->dst, ip->proto, payload, plen);
        return;
    }

    size_t   whole_len;
    uint8_t *whole = ipv4_reassemble(ip, payload, plen, &whole_len);
    if (whole) {
        ipv4_deliver(nif, ip->src, ip->dst, ip->proto, whole, whole_len);
        kfree(whole);
    }
}

netif_t *ipv4_route(uint32_t dst) {
    (void)dst;  // one interface for now
    return netif_default();
}

uint32_t ipv4_source(netif_t *nif, uint32_t dst) {
    return IP_LOOPBACK(dst) ? IP4(127, 0, 0, 1) : nif->ip;
}

// headed for us, it never touches the card
static bool ipv4_stays_here(netif_t *nif, uint32_t dst) {
    return IP_LOOPBACK(dst) || (nif->ip && dst == nif->ip);
}

// direct on our subnet, gateway otherwise. 0 when theres no route
static uint32_t ipv4_next_hop(netif_t *nif, uint32_t dst) {
    bool local = ipv4_stays_here(nif, dst) || dst == IP_BROADCAST ||
                 (nif->netmask && (dst & nif->netmask) == (nif->ip & nif->netmask));
    return local ? dst : nif->gateway;
}

static int ipv4_send(netif_t *nif, netbuf_t *nb, uint32_t src, uint32_t dst, uint8_t proto,
                     uint16_t id, uint16_t frag, uint32_t next_hop) {
    ipv4_hdr_t *ip = netbuf_push(nb, IP_HLEN);
    if (!ip) {
        netbuf_free(nb);
        return -ENOBUFS;
    }

    ip->ver_ihl   = 0x45;
    ip->tos       = 0;
    ip->total_len = htons((uint16_t)nb->len);
    ip->id        = htons(id);
    ip->frag      = htons(frag);
    ip->ttl       = IP_TTL;
    ip->proto     = proto;
    ip->checksum  = 0;
    ip->src       = src ? src : ipv4_source(nif, dst);
    ip->dst       = dst;
    ip->checksum  = net_checksum(ip, IP_HLEN, 0);

    if (ipv4_stays_here(nif, dst)) {
        ipv4_input(nif, nb->data, nb->len);
        netbuf_free(nb);
        return 0;
    }

    // a server on this machine wants to hear lan broadcasts too
    if (dst == IP_BROADCAST && nif->configured)
        ipv4_input(nif, nb->data, nb->len);

    return arp_output(nif, nb, next_hop);
}

int ipv4_output(netif_t *nif, netbuf_t *nb, uint32_t src, uint32_t dst, uint8_t proto) {
    if (!nif)
        nif = ipv4_route(dst);
    uint32_t next_hop = nif ? ipv4_next_hop(nif, dst) : 0;
    if (!next_hop) {
        netbuf_free(nb);
        return -ENETUNREACH;
    }

    uint16_t id = __atomic_fetch_add(&ip_next_id, 1, __ATOMIC_RELAXED);
    return ipv4_send(nif, nb, src, dst, proto, id, 0, next_hop);
}

int ipv4_output_flat(netif_t *nif, const void *payload, size_t len, uint32_t src, uint32_t dst, uint8_t proto) {
    if (len > IP_MAX_PAYLOAD)
        return -EMSGSIZE;
    if (!nif)
        nif = ipv4_route(dst);
    uint32_t next_hop = nif ? ipv4_next_hop(nif, dst) : 0;
    if (!next_hop)
        return -ENETUNREACH;

    uint16_t id = __atomic_fetch_add(&ip_next_id, 1, __ATOMIC_RELAXED);
    for (size_t off = 0; off < len || off == 0; off += IP_FRAG_CHUNK) {
        size_t    chunk = len - off < IP_FRAG_CHUNK ? len - off : IP_FRAG_CHUNK;
        bool      more  = off + chunk < len;
        netbuf_t *nb    = netbuf_alloc();
        if (!nb)
            return -ENOBUFS;

        memcpy(netbuf_put(nb, chunk), (const uint8_t *)payload + off, chunk);
        uint16_t frag = (uint16_t)(off / 8) | (more ? IP_FLAG_MF : 0);
        int      err  = ipv4_send(nif, nb, src, dst, proto, id, frag, next_hop);
        if (err)
            return err;
        if (!more)
            break;
    }
    return 0;
}
