#include <net/dhcp.h>
#include <net/udp.h>
#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

// only runs on the net service task
enum { DHCP_IDLE, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

typedef struct {
    netif_t  *nif;
    int       state;
    uint32_t  xid;
    int       tries;
    uint64_t  stamp;        // last packet we sent
    uint32_t  offered;
    uint32_t  server;
    uint64_t  renew_at;
    uint64_t  expire_at;
} dhcp_t;

static dhcp_t dhcp;

static uint8_t *dhcp_opt(uint8_t *p, uint8_t code, const void *data, uint8_t len) {
    p[0] = code;
    p[1] = len;
    memcpy(p + 2, data, len);
    return p + 2 + len;
}

static void dhcp_send(uint8_t type) {
    netif_t  *nif = dhcp.nif;
    netbuf_t *nb  = netbuf_alloc();
    if (!nb)
        return;

    dhcp_pkt_t *pkt = netbuf_put(nb, sizeof(dhcp_pkt_t));
    memset(pkt, 0, sizeof(*pkt));
    pkt->op    = DHCP_BOOTREQUEST;
    pkt->htype = 1;
    pkt->hlen  = ETH_ALEN;
    pkt->xid   = dhcp.xid;
    pkt->flags = htons(0x8000);     // broadcast, we have no address yet
    pkt->magic = htonl(DHCP_MAGIC);
    memcpy(pkt->chaddr, nif->mac, ETH_ALEN);
    if (dhcp.state == DHCP_RENEWING)
        pkt->ciaddr = nif->ip;

    uint8_t  opts[32];
    uint8_t *p      = opts;
    uint8_t  want[] = { DHCP_OPT_NETMASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS, DHCP_OPT_LEASE };

    p = dhcp_opt(p, DHCP_OPT_MSG_TYPE, &type, 1);
    if (type == DHCP_REQUEST && dhcp.state == DHCP_REQUESTING) {
        p = dhcp_opt(p, DHCP_OPT_REQ_IP, &dhcp.offered, 4);
        p = dhcp_opt(p, DHCP_OPT_SERVER_ID, &dhcp.server, 4);
    }
    p = dhcp_opt(p, DHCP_OPT_PARAMS, want, sizeof(want));
    *p++ = DHCP_OPT_END;

    size_t   olen = (size_t)(p - opts);
    uint8_t *tail = netbuf_put(nb, olen);
    memcpy(tail, opts, olen);

    dhcp.stamp = net_now_ms();
    dhcp.tries++;
    udp_output(nif, nb, 0, DHCP_CLIENT_PORT, IP_BROADCAST, DHCP_SERVER_PORT);
}

static void dhcp_discover(void) {
    dhcp.state = DHCP_SELECTING;
    dhcp.tries = 0;
    dhcp.xid   = htonl((uint32_t)hpet_read_counter() ^ ((uint32_t)dhcp.nif->mac[4] << 24 | (uint32_t)dhcp.nif->mac[5] << 16));
    serial_printf(LOG_INFO "dhcp: %s discover, xid 0x%08x\n", dhcp.nif->name, ntohl(dhcp.xid));
    dhcp_send(DHCP_DISCOVER);
}

static void dhcp_bind(const dhcp_pkt_t *pkt, uint32_t netmask, uint32_t router, uint32_t dns, uint32_t lease) {
    netif_t *nif = dhcp.nif;
    if (lease < DHCP_MIN_LEASE_S)
        lease = DHCP_MIN_LEASE_S;

    nif->ip         = pkt->yiaddr;
    nif->netmask    = netmask;
    nif->gateway    = router;
    nif->dns        = dns;
    nif->configured = true;

    uint64_t now   = net_now_ms();
    dhcp.state     = DHCP_BOUND;
    dhcp.renew_at  = now + (uint64_t)lease * 500;      // renew halfway through
    dhcp.expire_at = now + (uint64_t)lease * 1000;

    serial_printf(LOG_OK "dhcp: %s is " IP_FMT " mask " IP_FMT " gateway " IP_FMT " dns " IP_FMT ", lease %us\n",
                  nif->name, IP_ARGS(nif->ip), IP_ARGS(netmask), IP_ARGS(router), IP_ARGS(dns), lease);
}

static void dhcp_input(netif_t *nif, uint32_t src, uint16_t src_port, uint32_t dst, const void *data, size_t len) {
    (void)src; (void)src_port; (void)dst;
    if (nif != dhcp.nif || len < sizeof(dhcp_pkt_t))
        return;

    const dhcp_pkt_t *pkt = data;
    if (pkt->op != DHCP_BOOTREPLY || pkt->xid != dhcp.xid || ntohl(pkt->magic) != DHCP_MAGIC)
        return;

    uint8_t  type    = 0;
    uint32_t server  = 0, netmask = 0, router = 0, dns = 0, lease = 0;
    const uint8_t *p   = (const uint8_t *)(pkt + 1);
    const uint8_t *end = (const uint8_t *)data + len;

    while (p < end && *p != DHCP_OPT_END) {
        if (*p == DHCP_OPT_PAD) {
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end)
            break;

        uint8_t        code = p[0], olen = p[1];
        const uint8_t *val  = p + 2;
        switch (code) {
            case DHCP_OPT_MSG_TYPE:  if (olen >= 1) type = val[0]; break;
            case DHCP_OPT_SERVER_ID: if (olen >= 4) memcpy(&server, val, 4); break;
            case DHCP_OPT_NETMASK:   if (olen >= 4) memcpy(&netmask, val, 4); break;
            case DHCP_OPT_ROUTER:    if (olen >= 4) memcpy(&router, val, 4); break;
            case DHCP_OPT_DNS:       if (olen >= 4) memcpy(&dns, val, 4); break;
            case DHCP_OPT_LEASE:     if (olen >= 4) { memcpy(&lease, val, 4); lease = ntohl(lease); } break;
            default: break;
        }
        p += 2 + olen;
    }

    if (type == DHCP_OFFER && dhcp.state == DHCP_SELECTING) {
        serial_printf(LOG_INFO "dhcp: offer of " IP_FMT " from " IP_FMT "\n", IP_ARGS(pkt->yiaddr), IP_ARGS(server));
        dhcp.offered = pkt->yiaddr;
        dhcp.server  = server;
        dhcp.state   = DHCP_REQUESTING;
        dhcp.tries   = 0;
        dhcp_send(DHCP_REQUEST);
    } else if (type == DHCP_ACK && (dhcp.state == DHCP_REQUESTING || dhcp.state == DHCP_RENEWING)) {
        dhcp_bind(pkt, netmask, router, dns, lease);
    } else if (type == DHCP_NAK && dhcp.state != DHCP_BOUND) {
        serial_printf(LOG_WARN "dhcp: server said no, starting over\n");
        nif->configured = false;
        nif->ip         = 0;
        dhcp_discover();
    }
}

void dhcp_start(netif_t *nif) {
    if (!dhcp.nif) {
        int err = udp_listen(DHCP_CLIENT_PORT, dhcp_input);
        if (err) {
            serial_printf(LOG_ERROR "dhcp: couldnt listen on port %u (%d)\n", DHCP_CLIENT_PORT, err);
            return;
        }
    } else if (dhcp.nif != nif) {
        // TODO more than one interface
        serial_printf(LOG_WARN "dhcp: already running on %s, leaving %s alone\n", dhcp.nif->name, nif->name);
        return;
    }

    dhcp.nif = nif;
    dhcp_discover();
}

void dhcp_tick(void) {
    if (!dhcp.nif || dhcp.state == DHCP_IDLE)
        return;

    uint64_t now = net_now_ms();
    switch (dhcp.state) {
        case DHCP_SELECTING:
            if (now - dhcp.stamp >= DHCP_RETRY_MS)
                dhcp_send(DHCP_DISCOVER);
            break;
        case DHCP_REQUESTING:
            if (now - dhcp.stamp < DHCP_RETRY_MS)
                break;
            if (dhcp.tries >= 3)
                dhcp_discover();
            else
                dhcp_send(DHCP_REQUEST);
            break;
        case DHCP_BOUND:
            if (now >= dhcp.renew_at) {
                serial_printf(LOG_INFO "dhcp: renewing " IP_FMT "\n", IP_ARGS(dhcp.nif->ip));
                dhcp.state = DHCP_RENEWING;
                dhcp.tries = 0;
                dhcp_send(DHCP_REQUEST);
            }
            break;
        case DHCP_RENEWING:
            if (now >= dhcp.expire_at) {
                serial_printf(LOG_WARN "dhcp: lease on " IP_FMT " ran out\n", IP_ARGS(dhcp.nif->ip));
                dhcp.nif->configured = false;
                dhcp.nif->ip         = 0;
                dhcp_discover();
            } else if (now - dhcp.stamp >= DHCP_RETRY_MS) {
                dhcp_send(DHCP_REQUEST);
            }
            break;
    }
}
