#include <drivers/virtio/virtio_queue.h>
#include <drivers/virtio/virtio_pci.h>
#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

bool virtq_setup(virtio_pci_t *vp, virtq_t *q, uint16_t index) {
    memset(q, 0, sizeof(*q));
    virtio_write16(vp->common, VIRTIO_COMMON_Q_SELECT, index);

    uint16_t size = virtio_read16(vp->common, VIRTIO_COMMON_Q_SIZE);
    if (!size) {
        serial_printf(LOG_WARN "virtio: queue %u doesnt exist\n", index);
        return false;
    }

    // sizes are powers of two, halving keeps it one
    while (size > VIRTQ_MAX_SIZE)
        size /= 2;

    q->phys = pmm_alloc_pages(1);
    if (!q->phys) return false;

    uint8_t *page = paddr_to_vaddr(q->phys);
    memset(page, 0, PAGE_SIZE);

    q->index = index;
    q->size  = size;
    q->desc  = (volatile virtq_desc_t *)page;
    q->avail = (volatile virtq_avail_t *)(page + VIRTQ_AVAIL_OFF);
    q->used  = (volatile virtq_used_t *)(page + VIRTQ_USED_OFF);

    uint16_t noff = virtio_read16(vp->common, VIRTIO_COMMON_Q_NOFF);
    q->notify = (volatile uint16_t *)(vp->notify + (uint32_t)noff * vp->notify_mult);

    virtio_write16(vp->common, VIRTIO_COMMON_Q_SIZE, size);
    virtio_write16(vp->common, VIRTIO_COMMON_Q_MSIX, VIRTIO_MSI_NO_VECTOR);
    virtio_write64(vp->common, VIRTIO_COMMON_Q_DESC,  q->phys);
    virtio_write64(vp->common, VIRTIO_COMMON_Q_AVAIL, q->phys + VIRTQ_AVAIL_OFF);
    virtio_write64(vp->common, VIRTIO_COMMON_Q_USED,  q->phys + VIRTQ_USED_OFF);
    virtio_write16(vp->common, VIRTIO_COMMON_Q_ENABLE, 1);

    serial_printf(LOG_INFO "virtio: queue %u, %u entries at 0x%llx, notify offset %u\n",
                  index, size, (uint64_t)q->phys, noff);
    return true;
}

void virtq_give(virtq_t *q, uint16_t desc, paddr_t buf, uint32_t len) {
    q->desc[desc].addr  = buf;
    q->desc[desc].len   = len;
    q->desc[desc].flags = VIRTQ_DESC_F_WRITE;
    q->desc[desc].next  = 0;

    uint16_t idx = q->avail->idx;
    q->avail->ring[idx % q->size] = desc;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    q->avail->idx = idx + 1;
}

void virtq_kick(virtq_t *q) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify = q->index;
}

bool virtq_take(virtq_t *q, uint16_t *desc, uint32_t *len) {
    if (q->last_used == q->used->idx) return false;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    volatile virtq_used_elem_t *elem = &q->used->ring[q->last_used % q->size];
    *desc = (uint16_t)elem->id;
    *len  = elem->len;
    q->last_used++;
    return true;
}
