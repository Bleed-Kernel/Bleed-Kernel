#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <drivers/ahci/pci.h>

#define VIRTIO_PCI_VENDOR       0x1AF4
#define VIRTIO_PCI_DEVICE_BASE  0x1040  // modern devices are this plus the virtio device id

// vendor capabilities that say where each register block lives
#define PCI_CAP_ID_VENDOR       0x09
#define VIRTIO_CAP_COMMON       1
#define VIRTIO_CAP_NOTIFY       2
#define VIRTIO_CAP_ISR          3
#define VIRTIO_CAP_DEVICE       4

#define VIRTIO_CAP_TYPE         3       // offsets inside the capability
#define VIRTIO_CAP_BAR          4
#define VIRTIO_CAP_OFFSET       8
#define VIRTIO_CAP_LENGTH       12
#define VIRTIO_CAP_NOTIFY_MULT  16

// common config registers
#define VIRTIO_COMMON_DFSELECT  0x00
#define VIRTIO_COMMON_DF        0x04
#define VIRTIO_COMMON_GFSELECT  0x08
#define VIRTIO_COMMON_GF        0x0C
#define VIRTIO_COMMON_NUMQ      0x12
#define VIRTIO_COMMON_STATUS    0x14
#define VIRTIO_COMMON_Q_SELECT  0x16
#define VIRTIO_COMMON_Q_SIZE    0x18
#define VIRTIO_COMMON_Q_MSIX    0x1A
#define VIRTIO_COMMON_Q_ENABLE  0x1C
#define VIRTIO_COMMON_Q_NOFF    0x1E
#define VIRTIO_COMMON_Q_DESC    0x20
#define VIRTIO_COMMON_Q_AVAIL   0x28
#define VIRTIO_COMMON_Q_USED    0x30

#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEAT_OK   8
#define VIRTIO_STATUS_FAILED    128

#define VIRTIO_F_VERSION_1_HI   (1u << 0)   // feature bit 32, the only one we ask for
#define VIRTIO_MSI_NO_VECTOR    0xFFFF
#define VIRTIO_RESET_TIMEOUT_MS 100

typedef struct virtio_pci {
    pci_device_t      pci;
    volatile uint8_t *common;
    volatile uint8_t *notify;
    uint32_t          notify_mult;
    volatile uint8_t *device;   // the device type's own config
} virtio_pci_t;

/// @brief Find the register blocks, reset the device and agree on features
/// @param vp filled in
/// @param pci the device, from a pci scan
/// @return false if its not a modern virtio device or it wouldnt take our features
bool virtio_pci_init(virtio_pci_t *vp, const pci_device_t *pci);

// Tell the device the driver is done setting it up, queues have to be ready by now
void virtio_pci_ready(virtio_pci_t *vp);

// Give up on it, the device stops using anything we handed it
void virtio_pci_fail(virtio_pci_t *vp);

// common config access, everything is little endian which x86 already is
static inline uint8_t virtio_read8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static inline uint16_t virtio_read16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static inline uint32_t virtio_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void virtio_write8(volatile uint8_t *base, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(base + off) = val;
}

static inline void virtio_write16(volatile uint8_t *base, uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(base + off) = val;
}

static inline void virtio_write32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static inline void virtio_write64(volatile uint8_t *base, uint32_t off, uint64_t val) {
    virtio_write32(base, off, (uint32_t)val);
    virtio_write32(base, off + 4, (uint32_t)(val >> 32));
}
