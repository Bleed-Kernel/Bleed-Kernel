#include "xhci_priv.h"
#include <string.h>

// producer rings (4.9.2). whoever owns a trb is decided by its cycle bit matching
// the ring's, the link trb at the end sends the controller back to the start
// and flips what "ours" means so stale trbs from the last lap read as not ready

#define XHCI_RING_LINK  (XHCI_RING_TRBS - 1)

bool xhci_ring_init(xhci_hc_t *hc, xhci_ring_t *ring) {
    ring->trbs = usb_dma_alloc(1, &ring->phys, !hc->ac64);
    if (!ring->trbs)
        return false;

    ring->index = 0;
    ring->cycle = 1;

    // link cycle stays 0 until the first lap ends, the controller wont follow it before then
    xhci_trb_t *link = &ring->trbs[XHCI_RING_LINK];
    link->param   = ring->phys;
    link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC;
    return true;
}

void xhci_ring_free(xhci_ring_t *ring) {
    if (ring->trbs)
        pmm_free_pages(ring->phys, 1);
    memset(ring, 0, sizeof(*ring));
}

paddr_t xhci_ring_push(xhci_ring_t *ring, uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *trb  = &ring->trbs[ring->index];
    paddr_t     phys = ring->phys + ring->index * sizeof(xhci_trb_t);

    trb->param  = param;
    trb->status = status;
    // cycle bit last, the moment it matches the controller can take it
    asm volatile("" ::: "memory");
    trb->control = (control & ~XHCI_TRB_CYCLE) | ring->cycle;

    xhci_trace("xhci: trb %s at 0x%llx param=0x%llx status=0x%08x control=0x%08x\n",
               xhci_trb_name(XHCI_TRB_GET_TYPE(control)), (uint64_t)phys,
               param, status, trb->control);

    if (++ring->index == XHCI_RING_LINK) {
        // a TD that wraps keeps its chain going through the link
        xhci_trb_t *link = &ring->trbs[XHCI_RING_LINK];
        asm volatile("" ::: "memory");
        link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC
                      | (control & XHCI_TRB_CH) | ring->cycle;

        ring->index  = 0;
        ring->cycle ^= 1;
    }
    return phys;
}

// where the next trb goes plus the cycle it will have, what Set TR Dequeue wants
paddr_t xhci_ring_enqueue_ptr(const xhci_ring_t *ring) {
    return (ring->phys + ring->index * sizeof(xhci_trb_t)) | ring->cycle;
}
