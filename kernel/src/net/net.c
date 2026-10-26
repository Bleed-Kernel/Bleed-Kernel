#include <net/net.h>
#include <net/arp.h>
#include <net/dhcp.h>
#include <net/icmp.h>
#include <net/ipv4.h>
#include <net/tcp.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <sched/wait_queue.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>

static netif_t       netifs[NETIF_MAX];
static int           netif_count;
static wait_queue_t  net_wait;
static volatile bool net_kicked;

netbuf_t *netbuf_alloc(void) {
    netbuf_t *nb = kmalloc(sizeof(netbuf_t));
    if (!nb)
        return NULL;
    nb->data = nb->buf + NETBUF_HEADROOM;
    nb->len  = 0;
    return nb;
}

void netbuf_free(netbuf_t *nb) {
    if (nb) kfree(nb);
}

void *netbuf_push(netbuf_t *nb, size_t len) {
    if ((size_t)(nb->data - nb->buf) < len)
        return NULL;
    nb->data -= len;
    nb->len  += len;
    return nb->data;
}

void *netbuf_put(netbuf_t *nb, size_t len) {
    size_t used = (size_t)(nb->data - nb->buf) + nb->len;
    if (used + len > NETBUF_SIZE)
        return NULL;
    void *tail = nb->data + nb->len;
    nb->len += len;
    return tail;
}

netif_t *netif_register(const uint8_t mac[ETH_ALEN], const netif_ops_t *ops, void *driver) {
    if (netif_count >= NETIF_MAX)
        return NULL;

    netif_t *nif = &netifs[netif_count];
    memset(nif, 0, sizeof(*nif));
    nif->name[0] = 'e';
    nif->name[1] = 't';
    nif->name[2] = 'h';
    nif->name[3] = (char)('0' + netif_count);
    memcpy(nif->mac, mac, ETH_ALEN);
    nif->ops    = ops;
    nif->driver = driver;
    netif_count++;
    return nif;
}

netif_t *netif_get(int index) {
    return index >= 0 && index < netif_count ? &netifs[index] : NULL;
}

netif_t *netif_default(void) {
    for (int i = 0; i < netif_count; i++) {
        if (netifs[i].configured)
            return &netifs[i];
    }
    return netif_count ? &netifs[0] : NULL;
}

uint32_t net_checksum_partial(const void *data, size_t len, uint32_t start) {
    const uint8_t *p   = data;
    uint32_t       sum = start;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p   += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    return sum;
}

uint16_t net_checksum(const void *data, size_t len, uint32_t start) {
    uint32_t sum = net_checksum_partial(data, len, start);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint64_t net_now_ms(void) {
    return hpet_get_femtoseconds() / femtosecondsPerMillisecond;
}

void net_kick(void) {
    net_kicked = true;
    sched_wake_queue(&net_wait);
}

static void net_set_online(netif_t *nif, bool online) {
    if (nif->probed && nif->online == online)
        return;
    nif->probed = true;
    nif->online = online;
    if (online)
        serial_printf(LOG_OK "net: %s can reach the internet, " IP_FMT " answered in %ums\n",
                      nif->name, IP_ARGS(NET_PROBE_IP), nif->probe_rtt);
    else
        serial_printf(LOG_WARN "net: %s has no internet, " IP_FMT " didnt answer\n",
                      nif->name, IP_ARGS(NET_PROBE_IP));
}

void net_probe_answered(netif_t *nif) {
    if (!nif->probe_sent)
        return;
    uint64_t now   = net_now_ms();
    nif->probe_rtt  = (uint32_t)(now - nif->probe_sent);
    nif->probe_sent   = 0;
    nif->probe_at     = now + NET_PROBE_MS;
    nif->probe_misses = 0;
    net_set_online(nif, true);
}

static void net_probe_tick(netif_t *nif) {
    uint64_t now = net_now_ms();

    if (!nif->configured || !nif->link) {
        nif->probe_sent = 0;
        nif->probe_at   = 0;
        nif->probed       = false;
        nif->online       = false;
        nif->probe_misses = 0;
        return;
    }

    if (nif->probe_sent && now - nif->probe_sent >= NET_PROBE_WAIT_MS) {
        nif->probe_sent = 0;
        nif->probe_at   = now + NET_PROBE_RETRY_MS;
        serial_printf(LOG_INFO "net: internet check %u on %s went unanswered\n", nif->probe_seq, nif->name);
        if (++nif->probe_misses >= NET_PROBE_MISSES || !nif->probed)
            net_set_online(nif, false);
    }
    if (!nif->probe_sent && now >= nif->probe_at) {
        nif->probe_sent = now;
        icmp_ping(NET_PROBE_IP, ++nif->probe_seq);
    }
}

void net_service(void) {
    serial_printf(LOG_INFO "net: service running, %d interface(s)\n", netif_count);

    bool dhcp_started[NETIF_MAX] = {0};

    for (;;) {
        for (int i = 0; i < netif_count; i++) {
            netif_t *nif = &netifs[i];
            nif->ops->receive(nif);
            nif->link = nif->ops->link_up(nif);

            if (!dhcp_started[i] && nif->link) {
                dhcp_started[i] = true;
                dhcp_start(nif);
            }
            net_probe_tick(nif);
        }

        arp_tick();
        dhcp_tick();
        tcp_tick();
        ipv4_frag_tick();

        uint64_t      deadline = sched_deadline_ms(NET_SERVICE_MS);
        unsigned long flags    = irq_push();
        if (!net_kicked)
            sched_wait_queue(&net_wait, deadline);
        net_kicked = false;
        irq_restore(flags);
    }
}
