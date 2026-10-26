#include <drivers/e1000/e1000.h>
#include <drivers/ahci/pci.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/serial/serial.h>
#include <net/net.h>
#include <net/ether.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

typedef struct {
    pci_device_t     pci;
    uint64_t         mmio;
    netif_t         *nif;

    e1000_rx_desc_t *rx;
    uint8_t         *rx_bufs;
    uint32_t         rx_next;

    e1000_tx_desc_t *tx;
    uint8_t         *tx_bufs;
    uint32_t         tx_next;
    spinlock_t       tx_lock;

    bool             link;
    bool             kicked;
    uint64_t         tx_dropped;
} e1000_t;

static e1000_t e1000;
static bool    e1000_found;

static const uint16_t e1000_ids[] = {
    E1000_DEV_82540EM, E1000_DEV_82545EM, E1000_DEV_82543GC, E1000_DEV_82545GM,
};

// the usb pci/mmio helpers arent usb specific
static inline uint32_t e1000_read(e1000_t *e, uint32_t reg)             { return usb_mmio_read32(e->mmio, reg); }
static inline void     e1000_write(e1000_t *e, uint32_t reg, uint32_t v) { usb_mmio_write32(e->mmio, reg, v); }

static bool e1000_find(pci_device_t *out) {
    for (int i = 0; pci_find_device_nth(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0, i, out); i++) {
        if (out->vendor_id != PCI_VENDOR_INTEL) {
            serial_printf(LOG_INFO "e1000: skipping ethernet " PCI_SPEC " %04x:%04x, not intel\n",
                          PCI_SPEC_ARGS(out), out->vendor_id, out->device_id);
            continue;
        }
        for (size_t j = 0; j < sizeof(e1000_ids) / sizeof(e1000_ids[0]); j++) {
            if (out->device_id == e1000_ids[j])
                return true;
        }
        serial_printf(LOG_INFO "e1000: skipping intel ethernet " PCI_SPEC " device %04x, not one we know\n",
                      PCI_SPEC_ARGS(out), out->device_id);
    }
    return false;
}

static uint16_t e1000_eeprom_read(e1000_t *e, uint8_t word) {
    e1000_write(e, E1000_EERD, E1000_EERD_START | ((uint32_t)word << 8));
    if (!usb_wait32(e->mmio, E1000_EERD, E1000_EERD_DONE, true, 10)) {
        serial_printf(LOG_WARN "e1000: eeprom word %u timed out\n", word);
        return 0;
    }
    return (uint16_t)(e1000_read(e, E1000_EERD) >> 16);
}

// RAL0/RAH0 come from the eeprom on reset, only read it ourselves when they didnt
static void e1000_read_mac(e1000_t *e, uint8_t mac[ETH_ALEN]) {
    uint32_t ral = e1000_read(e, E1000_RAL0);
    uint32_t rah = e1000_read(e, E1000_RAH0);
    serial_printf(LOG_INFO "e1000: RAL0=0x%08x RAH0=0x%08x\n", ral, rah);

    if (rah & E1000_RAH_AV) {
        for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(ral >> (i * 8));
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
        return;
    }

    for (int i = 0; i < 3; i++) {
        uint16_t w = e1000_eeprom_read(e, (uint8_t)i);
        mac[i * 2]     = (uint8_t)w;
        mac[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    e1000_write(e, E1000_RAL0, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((uint32_t)mac[3] << 24));
    e1000_write(e, E1000_RAH0, mac[4] | (mac[5] << 8) | E1000_RAH_AV);
    serial_printf(LOG_INFO "e1000: mac came from the eeprom\n");
}

static bool e1000_reset(e1000_t *e) {
    e1000_write(e, E1000_IMC, 0xFFFFFFFF);
    e1000_write(e, E1000_RCTL, 0);
    e1000_write(e, E1000_TCTL, 0);

    e1000_write(e, E1000_CTRL, e1000_read(e, E1000_CTRL) | E1000_CTRL_RST);
    usb_delay_ms(1);
    if (!usb_wait32(e->mmio, E1000_CTRL, E1000_CTRL_RST, false, E1000_RESET_MS)) {
        serial_printf(LOG_ERROR "e1000: reset never finished, CTRL=0x%08x\n", e1000_read(e, E1000_CTRL));
        return false;
    }

    // masks come back on after a reset
    e1000_write(e, E1000_IMC, 0xFFFFFFFF);
    (void)e1000_read(e, E1000_ICR);

    uint32_t ctrl = e1000_read(e, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~(E1000_CTRL_LRST | E1000_CTRL_PHY_RST | E1000_CTRL_ILOS);
    e1000_write(e, E1000_CTRL, ctrl);
    serial_printf(LOG_INFO "e1000: reset done, CTRL=0x%08x STATUS=0x%08x\n",
                  e1000_read(e, E1000_CTRL), e1000_read(e, E1000_STATUS));
    return true;
}

static bool e1000_rx_init(e1000_t *e) {
    paddr_t ring_phys, bufs_phys;
    size_t  buf_pages = E1000_RX_DESCS * E1000_BUF_SIZE / PAGE_SIZE;

    e->rx      = usb_dma_alloc(1, &ring_phys, false);
    e->rx_bufs = usb_dma_alloc(buf_pages, &bufs_phys, false);
    if (!e->rx || !e->rx_bufs) {
        serial_printf(LOG_ERROR "e1000: out of memory for the rx ring\n");
        return false;
    }

    for (int i = 0; i < E1000_RX_DESCS; i++)
        e->rx[i].addr = bufs_phys + (uint64_t)i * E1000_BUF_SIZE;

    e1000_write(e, E1000_RDBAL, (uint32_t)ring_phys);
    e1000_write(e, E1000_RDBAH, (uint32_t)(ring_phys >> 32));
    e1000_write(e, E1000_RDLEN, E1000_RX_DESCS * sizeof(e1000_rx_desc_t));
    e1000_write(e, E1000_RDH, 0);
    e1000_write(e, E1000_RDT, E1000_RX_DESCS - 1);  // one slot stays empty so head never meets tail
    e->rx_next = 0;

    // nothing multicast for now, broadcast comes in through BAM
    for (int i = 0; i < 128; i++)
        e1000_write(e, E1000_MTA + i * 4, 0);

    e1000_write(e, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2K | E1000_RCTL_SECRC);
    serial_printf(LOG_INFO "e1000: rx ring %u descs at 0x%llx, buffers at 0x%llx\n",
                  E1000_RX_DESCS, (uint64_t)ring_phys, (uint64_t)bufs_phys);
    return true;
}

static bool e1000_tx_init(e1000_t *e) {
    paddr_t ring_phys, bufs_phys;
    size_t  buf_pages = E1000_TX_DESCS * E1000_BUF_SIZE / PAGE_SIZE;

    e->tx      = usb_dma_alloc(1, &ring_phys, false);
    e->tx_bufs = usb_dma_alloc(buf_pages, &bufs_phys, false);
    if (!e->tx || !e->tx_bufs) {
        serial_printf(LOG_ERROR "e1000: out of memory for the tx ring\n");
        return false;
    }

    // start done so the first lap isnt full
    for (int i = 0; i < E1000_TX_DESCS; i++) {
        e->tx[i].addr   = bufs_phys + (uint64_t)i * E1000_BUF_SIZE;
        e->tx[i].status = E1000_TXD_DD;
    }

    e1000_write(e, E1000_TDBAL, (uint32_t)ring_phys);
    e1000_write(e, E1000_TDBAH, (uint32_t)(ring_phys >> 32));
    e1000_write(e, E1000_TDLEN, E1000_TX_DESCS * sizeof(e1000_tx_desc_t));
    e1000_write(e, E1000_TDH, 0);
    e1000_write(e, E1000_TDT, 0);
    e->tx_next = 0;
    spinlock_init(&e->tx_lock);

    e1000_write(e, E1000_TIPG, E1000_TIPG_DEFAULT);
    e1000_write(e, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);
    serial_printf(LOG_INFO "e1000: tx ring %u descs at 0x%llx, buffers at 0x%llx\n",
                  E1000_TX_DESCS, (uint64_t)ring_phys, (uint64_t)bufs_phys);
    return true;
}

static int e1000_send(netif_t *nif, const void *frame, size_t len) {
    e1000_t *e = nif->driver;
    if (len > E1000_BUF_SIZE)
        return -EMSGSIZE;

    unsigned long flags = irq_push();
    spinlock_acquire(&e->tx_lock);

    uint32_t slot = e->tx_next;
    volatile e1000_tx_desc_t *d = &e->tx[slot];
    if (!(d->status & E1000_TXD_DD)) {
        e->tx_dropped++;
        spinlock_release(&e->tx_lock);
        irq_restore(flags);
        if (e->tx_dropped == 1 || e->tx_dropped % 100 == 0)
            serial_printf(LOG_WARN "e1000: tx ring full, %llu frames dropped so far\n", e->tx_dropped);
        return -EAGAIN;
    }

    memcpy(e->tx_bufs + (size_t)slot * E1000_BUF_SIZE, frame, len);
    d->length = (uint16_t)len;
    d->cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->status = 0;

    e->tx_next = (slot + 1) % E1000_TX_DESCS;
    asm volatile("" ::: "memory");
    e1000_write(e, E1000_TDT, e->tx_next);

    spinlock_release(&e->tx_lock);
    irq_restore(flags);
    return 0;
}

static void e1000_receive(netif_t *nif) {
    e1000_t *e = nif->driver;
    e->kicked = false;

    for (int n = 0; n < E1000_RX_DESCS; n++) {
        volatile e1000_rx_desc_t *d = &e->rx[e->rx_next];
        if (!(d->status & E1000_RXD_DD))
            break;
        asm volatile("" ::: "memory");

        // 2K fits any non jumbo frame
        if ((d->status & E1000_RXD_EOP) && !d->errors) {
            ether_input(nif, e->rx_bufs + (size_t)e->rx_next * E1000_BUF_SIZE, d->length);
        } else {
            serial_printf(LOG_WARN "e1000: dropping rx desc %u status=0x%02x errors=0x%02x\n",
                          e->rx_next, d->status, d->errors);
        }

        d->status = 0;
        e1000_write(e, E1000_RDT, e->rx_next);
        e->rx_next = (e->rx_next + 1) % E1000_RX_DESCS;
    }
}

static bool e1000_link_up(netif_t *nif) {
    e1000_t *e = nif->driver;
    bool up = !!(e1000_read(e, E1000_STATUS) & E1000_STATUS_LU);
    if (up != e->link) {
        e->link = up;
        serial_printf(LOG_INFO "e1000: link %s (STATUS=0x%08x)\n", up ? "up" : "down",
                      e1000_read(e, E1000_STATUS));
    }
    return up;
}

static const netif_ops_t e1000_ops = {
    .send    = e1000_send,
    .receive = e1000_receive,
    .link_up = e1000_link_up,
};

void e1000_poll(void) {
    if (!e1000_found || e1000.kicked)
        return;
    if (e1000.rx[e1000.rx_next].status & E1000_RXD_DD) {
        e1000.kicked = true;
        net_kick();
    }
}

void e1000_init(void) {
    e1000_t *e = &e1000;
    if (!e1000_find(&e->pci)) {
        serial_printf(LOG_INFO "e1000: no supported nic found\n");
        return;
    }
    serial_printf(LOG_INFO "e1000: found %04x:%04x at " PCI_SPEC " BAR0=0x%08x\n",
                  e->pci.vendor_id, e->pci.device_id, PCI_SPEC_ARGS(&e->pci), e->pci.bar[0]);

    if (e->pci.bar[0] & 1) {
        serial_printf(LOG_ERROR "e1000: BAR0 is io space, expected mmio\n");
        return;
    }

    uint64_t size = usb_bar0_size(&e->pci);
    usb_pci_enable(&e->pci);
    e->mmio = usb_map_bar0(&e->pci, size);
    if (!e->mmio) {
        serial_printf(LOG_ERROR "e1000: couldnt map BAR0 (%llu bytes)\n", size);
        return;
    }
    serial_printf(LOG_INFO "e1000: BAR0 mapped, %llu bytes\n", size);

    if (!e1000_reset(e) || !e1000_rx_init(e) || !e1000_tx_init(e))
        return;

    uint8_t mac[ETH_ALEN];
    e1000_read_mac(e, mac);

    e->nif = netif_register(mac, &e1000_ops, e);
    if (!e->nif) {
        serial_printf(LOG_ERROR "e1000: no room for another net interface\n");
        return;
    }
    e1000_found = true;
    e1000_link_up(e->nif);
    serial_printf(LOG_OK "e1000: %s ready, mac " MAC_FMT "\n", e->nif->name, MAC_ARGS(mac));
}
