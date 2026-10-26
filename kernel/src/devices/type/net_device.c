#include <devices/devices.h>
#include <devices/type/net_device.h>
#include <net/net.h>
#include <fs/vfs.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <sched/scheduler.h>
#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>

static const char *net_state_names[] = {
    "no network card", "unplugged", "connecting", "no internet", "online",
};

static uint8_t net_state(const netif_t *nif) {
    if (!nif)                               return NET_STATE_NONE;
    if (!nif->link)                         return NET_STATE_UNPLUGGED;
    if (!nif->configured || !nif->probed)   return NET_STATE_CONNECTING;
    return nif->online ? NET_STATE_ONLINE : NET_STATE_NO_INTERNET;
}

static void net_info_read(net_device_info_t *out) {
    netif_t *nif = netif_get(0);

    memset(out, 0, sizeof(*out));
    out->state = net_state(nif);
    if (!nif) return;

    memcpy(out->mac, nif->mac, ETH_ALEN);
    memcpy(out->name, nif->name, sizeof(out->name));
    out->ip       = nif->ip;
    out->netmask  = nif->netmask;
    out->gateway  = nif->gateway;
    out->dns      = nif->dns;
    out->rtt_ms   = nif->online ? nif->probe_rtt : 0;
    out->rx_bytes = nif->rx_bytes;
    out->tx_bytes = nif->tx_bytes;
}

// the same thing as text, so it can be looked at with cat
static size_t net_text(char *text, size_t max) {
    size_t len = 0;

    for (int i = 0; netif_get(i) && len < max; i++) {
        netif_t *nif = netif_get(i);
        len += (size_t)snprintf(text + len, max - len,
                                "%s: %s\n  mac " MAC_FMT "\n  ip " IP_FMT " mask " IP_FMT "\n"
                                "  gateway " IP_FMT " dns " IP_FMT "\n",
                                nif->name, net_state_names[net_state(nif)], MAC_ARGS(nif->mac),
                                IP_ARGS(nif->ip), IP_ARGS(nif->netmask), IP_ARGS(nif->gateway), IP_ARGS(nif->dns));
        if (len >= max) break;

        if (nif->online)
            len += (size_t)snprintf(text + len, max - len, "  internet yes, " IP_FMT " answered in %ums\n",
                                    IP_ARGS(NET_PROBE_IP), nif->probe_rtt);
        else
            len += (size_t)snprintf(text + len, max - len, "  internet %s\n", nif->probed ? "no" : "not checked yet");
        if (len >= max) break;

        len += (size_t)snprintf(text + len, max - len, "  rx %llu frames %llu bytes, tx %llu frames %llu bytes\n",
                                nif->rx_frames, nif->rx_bytes, nif->tx_frames, nif->tx_bytes);
    }

    if (!netif_get(0))
        len = (size_t)snprintf(text, max, "no network card\n");
    return len < max ? len : max - 1;
}

static long net_read(INode_t *inode, void *buf, size_t len, size_t offset) {
    (void)inode;
    if (!buf || len == 0) return 0;

    char   *text = kmalloc(NET_TEXT_MAX);
    if (!text) return -ENOMEM;
    size_t size = net_text(text, NET_TEXT_MAX);

    if (offset >= size) {
        kfree(text);
        return 0;
    }
    if (len > size - offset) len = size - offset;

    memcpy(buf, text + offset, len);
    kfree(text);
    return (long)len;
}

// without a size a read never comes to an end
static size_t net_size(INode_t *inode) {
    (void)inode;
    char *text = kmalloc(NET_TEXT_MAX);
    if (!text) return 0;
    size_t size = net_text(text, NET_TEXT_MAX);
    kfree(text);
    return size;
}

static int net_ioctl(INode_t *inode, unsigned long request, void *arg) {
    (void)inode;
    if (request != NET_DEVICE_INFO)
        return -EINVAL;
    if (!arg)
        return -EFAULT;

    net_device_info_t info;
    net_info_read(&info);

    task_t *task = get_current_task();
    if (!task || copy_to_user(task, arg, &info, sizeof(info)) != 0)
        return -EFAULT;
    return 0;
}

static INodeOps_t net_inode_ops = {
    .read  = net_read,
    .size  = net_size,
    .ioctl = net_ioctl,
};

void net_device_init(void) {
    INode_t *dev_inode = kmalloc(sizeof(INode_t));
    if (!dev_inode)
        return;

    memset(dev_inode, 0, sizeof(INode_t));
    dev_inode->ops    = &net_inode_ops;
    dev_inode->type   = INODE_DEVICE;
    dev_inode->shared = 1;

    device_register(dev_inode, "net");
}
