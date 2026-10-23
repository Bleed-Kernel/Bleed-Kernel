#include "xhci_priv.h"
#include <ACPI/acpi_hpet.h>
#include <string.h>

static void xhci_xfer_begin(xhci_ep_t *ep) {
    memset(&ep->xfer, 0, sizeof(ep->xfer));
}

static void xhci_xfer_push(xhci_ep_t *ep, uint64_t param, uint32_t len,
                           uint32_t status, uint32_t control) {
    xhci_xfer_t *x = &ep->xfer;
    x->trbs[x->count] = xhci_ring_push(&ep->ring, param, status, control);
    x->lens[x->count] = len;
    x->count++;
}

static int xhci_xfer_index(const xhci_xfer_t *x, paddr_t trb) {
    for (int i = 0; i < x->count; i++) {
        if (x->trbs[i] == trb)
            return i;
    }
    return -1;
}

// the controller halts an endpoint on these and wont run it again until its reset
static bool xhci_cc_halts(uint8_t cc) {
    return cc == XHCI_CC_STALL || cc == XHCI_CC_BABBLE || cc == XHCI_CC_TRANSACTION
        || cc == XHCI_CC_DATA_BUFFER || cc == 36;   // split transaction error
}

static void xhci_pipe_arm(xhci_slot_t *slot, xhci_ep_t *ep) {
    xhci_xfer_begin(ep);
    xhci_xfer_push(ep, ep->buf_phys, ep->max_packet, XHCI_TRB_LEN(ep->max_packet),
                   XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP | XHCI_TRB_IOC);
    xhci_doorbell(slot->hc, slot->id, ep->dci);
}

// interrupt pipes run off the event ring so they cant issue commands from in here,
// a halted pipe just stops. the controller already retried it 3 times by then
static void xhci_pipe_complete(xhci_slot_t *slot, xhci_ep_t *ep) {
    uint8_t cc = ep->xfer.cc;

    if (xhci_cc_ok(cc)) {
        ep->errors = 0;
        ep->cb(ep->ctx, ep->buf, ep->xfer.actual);
        xhci_pipe_arm(slot, ep);
        return;
    }

    serial_printf(LOG_WARN "xhci%d: slot %u interrupt dci %u -> %s (%u)\n",
                  slot->hc->index, slot->id, ep->dci, xhci_cc_name(cc), cc);

    if (ep->halted || ++ep->errors >= XHCI_PIPE_MAX_ERRORS) {
        // probably got unplugged, stop asking
        serial_printf(LOG_WARN "xhci%d: slot %u dci %u keeps failing, giving up on it\n",
                      slot->hc->index, slot->id, ep->dci);
        ep->cb = NULL;
        return;
    }
    xhci_pipe_arm(slot, ep);
}

void xhci_transfer_event(xhci_hc_t *hc, const xhci_trb_t *event) {
    uint8_t  slot_id  = XHCI_TRB_GET_SLOT(event->control);
    uint8_t  dci      = XHCI_TRB_GET_EP(event->control);
    uint8_t  cc       = XHCI_EVT_CC(event->status);
    uint32_t residual = XHCI_EVT_RESIDUAL(event->status);
    paddr_t  trb      = event->param;

    xhci_slot_t *slot = slot_id <= XHCI_MAX_SLOTS ? hc->slots[slot_id] : NULL;
    if (!slot || dci == 0 || dci >= XHCI_MAX_DCI) {
        serial_printf(LOG_WARN "xhci%d: transfer event for slot %u dci %u that we dont have\n",
                      hc->index, slot_id, dci);
        return;
    }

    xhci_ep_t   *ep = &slot->eps[dci];
    xhci_xfer_t *x  = &ep->xfer;
    if (xhci_cc_halts(cc))
        ep->halted = true;

    // real hardware reports the status stage after the data stage, qemu only
    // reports the first one. we finish on whichever comes first so the late one lands here
    int i = xhci_xfer_index(x, trb);
    if (i < 0 || x->done) {
        if (!xhci_cc_ok(cc))
            serial_printf(LOG_WARN "xhci%d: slot %u dci %u late event %s (%u)\n",
                          hc->index, slot_id, dci, xhci_cc_name(cc), cc);
        return;
    }

    uint32_t actual = 0;
    for (int n = 0; n < i; n++)
        actual += x->lens[n];
    actual += x->lens[i] > residual ? x->lens[i] - residual : 0;

    x->actual = actual;
    x->cc     = cc;
    x->done   = true;

    if (ep->cb)
        xhci_pipe_complete(slot, ep);
}

// returns the completion code, XHCI_CC_INVALID on a timeout
static uint8_t xhci_xfer_wait(xhci_slot_t *slot, xhci_ep_t *ep, uint64_t timeout_ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        xhci_event_poll(slot->hc);
        if (ep->xfer.done)
            return ep->xfer.cc;
        if (slot->dev.gone)
            return XHCI_CC_GONE;
        if (usb_timed_out(start, spins, timeout_ms))
            return XHCI_CC_INVALID;
        asm volatile("pause");
    }
}

// whatever was queued is abandoned, point the controller at the end of the ring.
// Set TR Dequeue only works on a stopped endpoint so get it there first
static int xhci_ep_skip(xhci_slot_t *slot, xhci_ep_t *ep) {
    xhci_hc_t *hc    = slot->hc;
    uint32_t  *out   = xhci_out_ctx(slot, ep->dci);
    uint8_t    state = XHCI_EP_GET_STATE(out[0]);

    if (state == XHCI_EP_STATE_HALTED && xhci_cmd_reset_ep(hc, slot, ep->dci) != XHCI_CC_SUCCESS)
        return USB_ERR;
    if (state == XHCI_EP_STATE_RUNNING && xhci_cmd_stop_ep(hc, slot, ep->dci) != XHCI_CC_SUCCESS)
        return USB_ERR;

    if (xhci_cmd_set_tr_dequeue(hc, slot, ep->dci, xhci_ring_enqueue_ptr(&ep->ring)) != XHCI_CC_SUCCESS)
        return USB_ERR;

    ep->halted = false;
    return 0;
}

static void xhci_xfer_log_fail(xhci_slot_t *slot, xhci_ep_t *ep, const char *what, uint8_t cc) {
    uint32_t *out = xhci_out_ctx(slot, ep->dci);
    if (cc == XHCI_CC_INVALID)
        serial_printf(LOG_WARN "xhci%d: slot %u %s timed out (ep state %u)\n",
                      slot->hc->index, slot->id, what, XHCI_EP_GET_STATE(out[0]));
    else
        serial_printf(LOG_WARN "xhci%d: slot %u %s -> %s (%u)\n",
                      slot->hc->index, slot->id, what, xhci_cc_name(cc), cc);
}

// setup -> (data) -> status as one TD on ep0 (4.11.2.2)
int xhci_control(usb_device_t *dev, const usb_setup_t *setup, void *data) {
    xhci_slot_t *slot = dev->hc_priv;
    xhci_ep_t   *ep   = &slot->eps[1];
    uint16_t     len  = setup->length;
    bool         in   = setup->request_type & USB_REQ_DIR_IN;

    if (len > PAGE_SIZE || (len && !data))
        return USB_ERR;

    // a stall on ep0 only means the device didnt like that request, clear it and carry on
    if (ep->halted && xhci_ep_skip(slot, ep) < 0)
        return USB_ERR;

    if (len && !in)
        memcpy(slot->ctrl_buf, data, len);

    uint64_t packet;
    memcpy(&packet, setup, sizeof(packet));
    uint32_t trt = !len ? XHCI_TRB_TRT_NONE : in ? XHCI_TRB_TRT_IN : XHCI_TRB_TRT_OUT;

    xhci_xfer_begin(ep);
    xhci_xfer_push(ep, packet, 0, XHCI_TRB_LEN(8),
                   XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT | trt);
    if (len)
        xhci_xfer_push(ep, slot->ctrl_buf_phys, len, XHCI_TRB_LEN(len),
                       XHCI_TRB_TYPE(XHCI_TRB_DATA) | XHCI_TRB_ISP | XHCI_TRB_IOC
                       | (in ? XHCI_TRB_DIR_IN : 0));
    // status goes the opposite way to the data, IN when there wasnt any
    xhci_xfer_push(ep, 0, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC
                   | ((len && in) ? 0 : XHCI_TRB_DIR_IN));
    xhci_doorbell(slot->hc, slot->id, 1);

    uint8_t cc = xhci_xfer_wait(slot, ep, XHCI_CTRL_TIMEOUT_MS);
    if (cc == XHCI_CC_GONE)
        return USB_ERR_GONE;
    if (!xhci_cc_ok(cc)) {
        char what[48];
        snprintf(what, sizeof(what), "control %02x:%02x", setup->request_type, setup->request);
        xhci_xfer_log_fail(slot, ep, what, cc);

        xhci_ep_skip(slot, ep);
        return cc == XHCI_CC_STALL ? USB_ERR_STALL : cc == XHCI_CC_INVALID ? USB_ERR_TIMEOUT : USB_ERR;
    }

    uint32_t got = ep->xfer.actual;
    if (in && got)
        memcpy(data, slot->ctrl_buf, got);
    return (int)got;
}

static int xhci_interrupt_in(usb_device_t *dev, const usb_endpoint_desc_t *desc,
                             usb_interrupt_cb_t cb, void *ctx) {
    xhci_slot_t *slot = dev->hc_priv;
    xhci_ep_t   *ep   = &slot->eps[xhci_dci(desc->address)];

    if (xhci_endpoint_add(slot, desc, 0) < 0)
        return USB_ERR;

    ep->buf = usb_dma_alloc(1, &ep->buf_phys, !slot->hc->ac64);
    if (!ep->buf)
        return USB_ERR;

    ep->errors = 0;
    ep->ctx    = ctx;
    ep->cb     = cb;

    // the event handler can run the moment the doorbell goes, keep it from racing us
    unsigned long irq = irq_push();
    xhci_pipe_arm(slot, ep);
    irq_restore(irq);
    return 0;
}

static int xhci_bulk_open(usb_device_t *dev, const usb_endpoint_desc_t *desc,
                          const usb_ss_companion_desc_t *ss) {
    return xhci_endpoint_add(dev->hc_priv, desc, ss ? ss->max_burst : 0);
}

// a TD for the whole buffer, split wherever it would cross a 64K boundary (6.4.1).
// TD size is how many packets are left after each trb, the controller uses it to plan bursts
static int xhci_bulk(usb_device_t *dev, uint8_t endpoint, void *buf, size_t len) {
    xhci_slot_t *slot = dev->hc_priv;
    xhci_ep_t   *ep   = &slot->eps[xhci_dci(endpoint)];

    if (!ep->used || (ep->type != XHCI_EP_TYPE_BULK_IN && ep->type != XHCI_EP_TYPE_BULK_OUT))
        return USB_ERR;
    if (ep->halted)
        return USB_ERR_STALL;

    paddr_t phys    = vaddr_to_paddr(buf);
    size_t  left    = len;
    size_t  packets = (len + ep->max_packet - 1) / ep->max_packet;
    size_t  moved   = 0;

    // count first, a half queued TD would still belong to the controller
    size_t trbs = 0;
    for (paddr_t p = phys; p < phys + len; p = (p | (XHCI_TRB_MAX_LEN - 1)) + 1)
        trbs++;
    if (trbs == 0 || trbs > XHCI_XFER_MAX_TRBS) {
        serial_printf(LOG_ERROR "xhci%d: bulk transfer of %llu bytes needs %llu trbs, we do 1-%u\n",
                      slot->hc->index, (uint64_t)len, (uint64_t)trbs, XHCI_XFER_MAX_TRBS);
        return USB_ERR;
    }

    xhci_xfer_begin(ep);
    do {
        uint32_t chunk = XHCI_TRB_MAX_LEN - (uint32_t)(phys & (XHCI_TRB_MAX_LEN - 1));
        if (chunk > left)
            chunk = (uint32_t)left;
        left  -= chunk;
        moved += chunk;

        size_t   done_packets = moved / ep->max_packet;
        uint32_t td_size      = left ? (uint32_t)(packets - done_packets) : 0;
        uint32_t flags        = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_ISP
                              | (left ? XHCI_TRB_CH : XHCI_TRB_IOC);

        xhci_xfer_push(ep, phys, chunk, XHCI_TRB_LEN(chunk) | XHCI_TRB_TD_SIZE(td_size), flags);
        phys += chunk;
    } while (left);

    xhci_doorbell(slot->hc, slot->id, ep->dci);

    uint8_t cc = xhci_xfer_wait(slot, ep, XHCI_BULK_TIMEOUT_MS);
    if (cc == XHCI_CC_GONE)
        return USB_ERR_GONE;
    if (cc == XHCI_CC_STALL) {
        // the class driver decides what a stall means, it calls usb_clear_halt
        xhci_xfer_log_fail(slot, ep, "bulk", cc);
        return USB_ERR_STALL;
    }
    if (!xhci_cc_ok(cc)) {
        xhci_xfer_log_fail(slot, ep, "bulk", cc);
        xhci_ep_skip(slot, ep);
        return cc == XHCI_CC_INVALID ? USB_ERR_TIMEOUT : USB_ERR;
    }

    xhci_trace("xhci%d: slot %u bulk 0x%02x moved %u of %llu bytes\n", slot->hc->index,
               slot->id, endpoint, ep->xfer.actual, (uint64_t)len);
    return (int)ep->xfer.actual;
}

static int xhci_reset_endpoint(usb_device_t *dev, uint8_t endpoint) {
    xhci_slot_t *slot = dev->hc_priv;
    xhci_ep_t   *ep   = &slot->eps[xhci_dci(endpoint)];
    if (!ep->used)
        return USB_ERR;

    // only a halted endpoint needs anything, the device side might be the only one stuck
    uint32_t *out = xhci_out_ctx(slot, ep->dci);
    if (XHCI_EP_GET_STATE(out[0]) != XHCI_EP_STATE_HALTED && !ep->halted)
        return 0;
    return xhci_ep_skip(slot, ep);
}

const usb_hc_ops_t xhci_ops = {
    .name           = "xhci",
    .control        = xhci_control,
    .interrupt_in   = xhci_interrupt_in,
    .bulk_open      = xhci_bulk_open,
    .bulk           = xhci_bulk,
    .reset_endpoint = xhci_reset_endpoint,
};
