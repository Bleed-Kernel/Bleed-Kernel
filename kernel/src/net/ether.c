#include <net/ether.h>
#include <net/arp.h>
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

const uint8_t eth_broadcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void ether_input(netif_t *nif, const void *frame, size_t len) {
    if (len < ETH_HLEN)
        return;

    const eth_hdr_t *eth = frame;
    nif->rx_frames++;
    nif->rx_bytes += len;
    if (memcmp(eth->dst, nif->mac, ETH_ALEN) != 0 && memcmp(eth->dst, eth_broadcast, ETH_ALEN) != 0)
        return;

    const uint8_t *payload = (const uint8_t *)frame + ETH_HLEN;
    size_t         plen    = len - ETH_HLEN;

    switch (ntohs(eth->type)) {
        case ETH_TYPE_ARP:
            arp_input(nif, payload, plen);
            break;
        case ETH_TYPE_IPV4:
            ipv4_input(nif, payload, plen);
            break;
        default:
            break;  // ipv6 and friends, nobody here speaks them
    }
}

int ether_output(netif_t *nif, netbuf_t *nb, const uint8_t dst[ETH_ALEN], uint16_t type) {
    eth_hdr_t *eth = netbuf_push(nb, ETH_HLEN);
    if (!eth) {
        netbuf_free(nb);
        return -ENOBUFS;
    }

    memcpy(eth->dst, dst, ETH_ALEN);
    memcpy(eth->src, nif->mac, ETH_ALEN);
    eth->type = htons(type);

    int err = nif->ops->send(nif, nb->data, nb->len);
    if (!err) {
        nif->tx_frames++;
        nif->tx_bytes += nb->len;
    }
    netbuf_free(nb);
    return err;
}
