#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <drivers/serial/serial.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

#define ICMP_PING_ID    0xB1ED
#define ICMP_PING_DATA  32

static uint64_t ping_sent_ms;

static int icmp_send(netif_t *nif, uint32_t dst, uint8_t type, uint16_t id, uint16_t seq,
                     const void *data, size_t len) {
    netbuf_t *nb = netbuf_alloc();
    if (!nb)
        return -ENOMEM;

    icmp_hdr_t *icmp = netbuf_put(nb, sizeof(icmp_hdr_t) + len);
    if (!icmp) {
        netbuf_free(nb);
        return -EMSGSIZE;
    }

    icmp->type     = type;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = id;
    icmp->seq      = seq;
    if (data)
        memcpy(icmp + 1, data, len);
    else
        memset(icmp + 1, 0, len);
    icmp->checksum = net_checksum(icmp, nb->len, 0);

    return ipv4_output(nif, nb, 0, dst, IP_PROTO_ICMP);
}

// the quoted header says which of our segments this is about
static void icmp_unreachable(uint8_t code, const void *data, size_t len) {
    if (len < IP_HLEN)
        return;

    const ipv4_hdr_t *ip   = data;
    size_t            hlen = (size_t)(ip->ver_ihl & 0x0F) * 4;
    if (hlen < IP_HLEN || len < hlen + 8 || ip->proto != IP_PROTO_TCP)
        return;

    const uint8_t *tcp = (const uint8_t *)data + hlen;
    uint16_t src_port, dst_port;
    uint32_t seq;
    memcpy(&src_port, tcp, 2);
    memcpy(&dst_port, tcp + 2, 2);
    memcpy(&seq, tcp + 4, 4);
    tcp_unreachable(code, ip->src, ntohs(src_port), ip->dst, ntohs(dst_port), ntohl(seq));
}

void icmp_input(netif_t *nif, uint32_t src, const void *data, size_t len) {
    if (len < sizeof(icmp_hdr_t) || net_checksum(data, len, 0) != 0)
        return;

    const icmp_hdr_t *icmp = data;
    switch (icmp->type) {
        case ICMP_ECHO_REQUEST:
            icmp_send(nif, src, ICMP_ECHO_REPLY, icmp->id, icmp->seq, icmp + 1, len - sizeof(icmp_hdr_t));
            break;
        case ICMP_ECHO_REPLY:
            if (ntohs(icmp->id) != ICMP_PING_ID)
                break;
            if (src == NET_PROBE_IP)
                net_probe_answered(nif);
            else
                serial_printf(LOG_OK "icmp: reply from " IP_FMT " seq=%u time=%llums\n",
                              IP_ARGS(src), ntohs(icmp->seq), net_now_ms() - ping_sent_ms);
            break;
        case ICMP_UNREACHABLE:
            serial_printf(LOG_INFO "icmp: " IP_FMT " says unreachable, code %u\n", IP_ARGS(src), icmp->code);
            icmp_unreachable(icmp->code, icmp + 1, len - sizeof(icmp_hdr_t));
            break;
        default:
            break;
    }
}

int icmp_ping(uint32_t dst, uint16_t seq) {
    if (dst != NET_PROBE_IP)
        serial_printf(LOG_INFO "icmp: ping " IP_FMT " seq=%u\n", IP_ARGS(dst), seq);
    ping_sent_ms = net_now_ms();
    return icmp_send(NULL, dst, ICMP_ECHO_REQUEST, htons(ICMP_PING_ID), htons(seq), NULL, ICMP_PING_DATA);
}
