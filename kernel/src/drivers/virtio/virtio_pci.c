#include <drivers/virtio/virtio_pci.h>
#include <drivers/ahci/pci.h>
#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static uint64_t virtio_bar_phys(const pci_device_t *pci, uint8_t bar) {
    if (bar > 5) return 0;

    uint32_t lo = pci->bar[bar];
    if (lo & 1) return 0;   // io space, modern devices dont use it for these

    uint64_t phys = lo & ~0xFull;
    if ((lo & 0x6) == 0x4 && bar < 5)
        phys |= (uint64_t)pci->bar[bar + 1] << 32;
    return phys;
}

static volatile uint8_t *virtio_map_cap(const pci_device_t *pci, uint8_t cap) {
    uint8_t  b      = pci->bus, d = pci->device, f = pci->function;
    uint8_t  bar    = pci_read8(b, d, f, cap + VIRTIO_CAP_BAR);
    uint32_t offset = pci_read32(b, d, f, cap + VIRTIO_CAP_OFFSET);
    uint32_t length = pci_read32(b, d, f, cap + VIRTIO_CAP_LENGTH);

    uint64_t phys = virtio_bar_phys(pci, bar);
    if (!phys || !length) return NULL;

    phys += offset;
    if (paging_map_mmio(phys, length) < 0) return NULL;
    return (volatile uint8_t *)paddr_to_vaddr(phys);
}

// walk the capability list for the vendor ones, each says which bar and where in it
static bool virtio_find_caps(virtio_pci_t *vp) {
    const pci_device_t *pci = &vp->pci;
    uint8_t b = pci->bus, d = pci->device, f = pci->function;

    if (!(pci_read16(b, d, f, PCI_STATUS) & PCI_STATUS_CAP_LIST))
        return false;

    uint8_t cap = pci_read8(b, d, f, PCI_CAP_POINTER) & ~3;
    for (int guard = 0; cap && guard < 48; guard++) {
        if (pci_read8(b, d, f, cap) == PCI_CAP_ID_VENDOR) {
            uint8_t type = pci_read8(b, d, f, cap + VIRTIO_CAP_TYPE);

            if (type == VIRTIO_CAP_COMMON && !vp->common) {
                vp->common = virtio_map_cap(pci, cap);
            } else if (type == VIRTIO_CAP_NOTIFY && !vp->notify) {
                vp->notify      = virtio_map_cap(pci, cap);
                vp->notify_mult = pci_read32(b, d, f, cap + VIRTIO_CAP_NOTIFY_MULT);
            } else if (type == VIRTIO_CAP_DEVICE && !vp->device) {
                vp->device = virtio_map_cap(pci, cap);
            }
        }
        cap = pci_read8(b, d, f, cap + 1) & ~3;
    }

    return vp->common && vp->notify && vp->device;
}

static void virtio_set_status(virtio_pci_t *vp, uint8_t bits) {
    uint8_t status = virtio_read8(vp->common, VIRTIO_COMMON_STATUS);
    virtio_write8(vp->common, VIRTIO_COMMON_STATUS, status | bits);
}

bool virtio_pci_init(virtio_pci_t *vp, const pci_device_t *pci) {
    memset(vp, 0, sizeof(*vp));
    vp->pci = *pci;

    uint8_t  b   = pci->bus, d = pci->device, f = pci->function;
    uint16_t cmd = pci_read16(b, d, f, PCI_COMMAND);
    pci_write16(b, d, f, PCI_COMMAND, cmd | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER);

    if (!virtio_find_caps(vp)) {
        serial_printf(LOG_WARN "virtio: %02x:%02x.%u has no modern register blocks (common %p notify %p device %p)\n",
                      b, d, f, vp->common, vp->notify, vp->device);
        return false;
    }

    // writing 0 resets it, its done once it reads back 0
    virtio_write8(vp->common, VIRTIO_COMMON_STATUS, 0);
    uint64_t start = hpet_get_femtoseconds();
    while (virtio_read8(vp->common, VIRTIO_COMMON_STATUS)) {
        if (hpet_get_femtoseconds() - start > VIRTIO_RESET_TIMEOUT_MS * 1000000000000ull) {
            serial_printf(LOG_WARN "virtio: %02x:%02x.%u never finished resetting\n", b, d, f);
            return false;
        }
        asm volatile("pause");
    }

    virtio_set_status(vp, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    virtio_write32(vp->common, VIRTIO_COMMON_DFSELECT, 1);
    uint32_t hi = virtio_read32(vp->common, VIRTIO_COMMON_DF);
    virtio_write32(vp->common, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t lo = virtio_read32(vp->common, VIRTIO_COMMON_DF);
    serial_printf(LOG_INFO "virtio: %02x:%02x.%u features 0x%08x%08x, %u queue(s)\n",
                  b, d, f, hi, lo, virtio_read16(vp->common, VIRTIO_COMMON_NUMQ));

    if (!(hi & VIRTIO_F_VERSION_1_HI)) {
        serial_printf(LOG_WARN "virtio: %02x:%02x.%u doesnt offer VERSION_1\n", b, d, f);
        virtio_pci_fail(vp);
        return false;
    }

    virtio_write32(vp->common, VIRTIO_COMMON_GFSELECT, 0);
    virtio_write32(vp->common, VIRTIO_COMMON_GF, 0);
    virtio_write32(vp->common, VIRTIO_COMMON_GFSELECT, 1);
    virtio_write32(vp->common, VIRTIO_COMMON_GF, VIRTIO_F_VERSION_1_HI);

    virtio_set_status(vp, VIRTIO_STATUS_FEAT_OK);
    if (!(virtio_read8(vp->common, VIRTIO_COMMON_STATUS) & VIRTIO_STATUS_FEAT_OK)) {
        serial_printf(LOG_WARN "virtio: %02x:%02x.%u turned down our features\n", b, d, f);
        virtio_pci_fail(vp);
        return false;
    }
    return true;
}

void virtio_pci_ready(virtio_pci_t *vp) {
    virtio_set_status(vp, VIRTIO_STATUS_DRIVER_OK);
}

void virtio_pci_fail(virtio_pci_t *vp) {
    if (vp->common)
        virtio_set_status(vp, VIRTIO_STATUS_FAILED);
}
