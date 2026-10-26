#include <net/arp.h>
#include <net/ether.h>
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <mm/spinlock.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

enum { ARP_FREE, ARP_PENDING, ARP_RESOLVED };

typedef struct {
    uint32_t  ip;
    uint8_t   mac[ETH_ALEN];
    uint8_t   state;
    uint8_t   tries;
    uint64_t  stamp;        // last request sent while pending, when it resolved after
    netif_t  *nif;
    netbuf_t *pending;      // we only should keep the newest one
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static spinlock_t  arp_lock;

static arp_entry_t *arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].state != ARP_FREE && arp_cache[i].ip == ip)
            return &arp_cache[i];
    }
    return NULL;
}

// a free slot, or the oldest resolved one. NULL when everything is mid lookup
static arp_entry_t *arp_slot(void) {
    arp_entry_t *oldest = NULL;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_entry_t *a = &arp_cache[i];
        if (a->state == ARP_FREE)
            return a;
        if (a->state == ARP_RESOLVED && (!oldest || a->stamp < oldest->stamp))
            oldest = a;
    }
    return oldest;
}

static int arp_send(netif_t *nif, uint16_t op, const uint8_t *target_mac, uint32_t target_ip) {
    netbuf_t *nb = netbuf_alloc();
    if (!nb)
        return -ENOMEM;

    arp_pkt_t *arp = netbuf_put(nb, sizeof(arp_pkt_t));
    arp->hw_type    = htons(ARP_HW_ETHER);
    arp->proto_type = htons(ETH_TYPE_IPV4);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->op         = htons(op);
    memcpy(arp->sender_mac, nif->mac, ETH_ALEN);
    arp->sender_ip  = nif->ip;
    if (target_mac)
        memcpy(arp->target_mac, target_mac, ETH_ALEN);
    else
        memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip  = target_ip;

    return ether_output(nif, nb, op == ARP_OP_REPLY ? target_mac : eth_broadcast, ETH_TYPE_ARP);
}

void arp_input(netif_t *nif, const void *data, size_t len) {
    if (len < sizeof(arp_pkt_t))
        return;

    const arp_pkt_t *arp = data;
    if (ntohs(arp->hw_type) != ARP_HW_ETHER || ntohs(arp->proto_type) != ETH_TYPE_IPV4 ||
        arp->hw_len != ETH_ALEN || arp->proto_len != 4)
        return;

    uint32_t  sender  = arp->sender_ip;
    bool      for_us  = nif->ip && arp->target_ip == nif->ip;
    netbuf_t *flush   = NULL;
    uint8_t   mac[ETH_ALEN];
    memcpy(mac, arp->sender_mac, ETH_ALEN);

    unsigned long flags = irq_push();
    spinlock_acquire(&arp_lock);

    // refresh who we know, only add them if they asked us
    arp_entry_t *a = sender ? arp_find(sender) : NULL;
    if (!a && sender && for_us)
        a = arp_slot();

    if (a) {
        bool was_pending = a->state == ARP_PENDING;
        a->ip    = sender;
        a->nif   = nif;
        a->state = ARP_RESOLVED;
        a->stamp = net_now_ms();
        memcpy(a->mac, mac, ETH_ALEN);
        flush      = a->pending;
        a->pending = NULL;
        if (was_pending)
            serial_printf(LOG_INFO "arp: " IP_FMT " is at " MAC_FMT "\n", IP_ARGS(sender), MAC_ARGS(mac));
    }

    spinlock_release(&arp_lock);
    irq_restore(flags);

    if (flush)
        ether_output(nif, flush, mac, ETH_TYPE_IPV4);

    if (for_us && ntohs(arp->op) == ARP_OP_REQUEST)
        arp_send(nif, ARP_OP_REPLY, mac, sender);
}

int arp_output(netif_t *nif, netbuf_t *nb, uint32_t next_hop) {
    uint32_t subnet_bcast = (nif->ip & nif->netmask) | ~nif->netmask;
    if (next_hop == IP_BROADCAST || (nif->netmask && next_hop == subnet_bcast))
        return ether_output(nif, nb, eth_broadcast, ETH_TYPE_IPV4);

    unsigned long flags = irq_push();
    spinlock_acquire(&arp_lock);

    arp_entry_t *a = arp_find(next_hop);
    if (a && a->state == ARP_RESOLVED) {
        uint8_t mac[ETH_ALEN];
        memcpy(mac, a->mac, ETH_ALEN);
        spinlock_release(&arp_lock);
        irq_restore(flags);
        return ether_output(nif, nb, mac, ETH_TYPE_IPV4);
    }

    if (a) {
        netbuf_t *old = a->pending;
        a->pending = nb;
        spinlock_release(&arp_lock);
        irq_restore(flags);
        netbuf_free(old);
        return 0;
    }

    a = arp_slot();
    if (!a) {
        spinlock_release(&arp_lock);
        irq_restore(flags);
        serial_printf(LOG_WARN "arp: cache full of lookups, dropping packet for " IP_FMT "\n", IP_ARGS(next_hop));
        netbuf_free(nb);
        return -ENOBUFS;
    }

    netbuf_t *evicted = a->pending;
    a->ip      = next_hop;
    a->nif     = nif;
    a->state   = ARP_PENDING;
    a->tries   = 1;
    a->stamp   = net_now_ms();
    a->pending = nb;
    spinlock_release(&arp_lock);
    irq_restore(flags);

    netbuf_free(evicted);
    serial_printf(LOG_INFO "arp: who has " IP_FMT "\n", IP_ARGS(next_hop));
    arp_send(nif, ARP_OP_REQUEST, NULL, next_hop);
    return 0;
}

void arp_tick(void) {
    uint64_t now = net_now_ms();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_entry_t *a = &arp_cache[i];
        netif_t     *resend = NULL;
        netbuf_t    *dropped = NULL;

        unsigned long flags = irq_push();
        spinlock_acquire(&arp_lock);
        uint32_t ip = a->ip;

        if (a->state == ARP_PENDING && now - a->stamp >= ARP_RETRY_MS) {
            if (a->tries >= ARP_TRIES) {
                dropped    = a->pending;
                a->pending = NULL;
                a->state   = ARP_FREE;
            } else {
                a->tries++;
                a->stamp = now;
                resend   = a->nif;
            }
        } else if (a->state == ARP_RESOLVED && now - a->stamp >= ARP_TTL_MS) {
            a->state = ARP_FREE;
        }

        spinlock_release(&arp_lock);
        irq_restore(flags);

        if (dropped) {
            serial_printf(LOG_WARN "arp: no answer from " IP_FMT " after %d tries\n", IP_ARGS(ip), ARP_TRIES);
            netbuf_free(dropped);
        }
        if (resend)
            arp_send(resend, ARP_OP_REQUEST, NULL, ip);
    }
}
