#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <drivers/virtio/virtio_pci.h>
#include <mm/pmm.h>

#define VIRTQ_MAX_SIZE      64      // desc, avail and used for this many all fit in one page
#define VIRTQ_DESC_F_WRITE  2       // the device writes into this buffer

#define VIRTQ_AVAIL_OFF     1024
#define VIRTQ_USED_OFF      2048

typedef struct __attribute__((packed)) virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} virtq_avail_t;

typedef struct __attribute__((packed)) virtq_used_elem {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} virtq_used_t;

typedef struct virtq {
    uint16_t                index;      // queue number on the device
    uint16_t                size;
    uint16_t                last_used;  // used ring entries we already looked at
    paddr_t                 phys;
    volatile virtq_desc_t  *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t  *used;
    volatile uint16_t      *notify;
} virtq_t;

/// @brief Allocate a queue and tell the device where it is
/// @param vp the device, between virtio_pci_init and virtio_pci_ready
/// @param q filled in
/// @param index which of the devices queues
/// @return false if the device doesnt have it or theres no memory
bool virtq_setup(virtio_pci_t *vp, virtq_t *q, uint16_t index);

// Hand a buffer the device can write into, call virtq_kick once theyre all in
void virtq_give(virtq_t *q, uint16_t desc, paddr_t buf, uint32_t len);

// Let the device know theres new buffers
void virtq_kick(virtq_t *q);

// Next buffer the device finished with, false when it hasnt finished any more
bool virtq_take(virtq_t *q, uint16_t *desc, uint32_t *len);
