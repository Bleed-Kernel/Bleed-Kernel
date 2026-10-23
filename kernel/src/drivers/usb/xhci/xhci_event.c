#include "xhci_priv.h"
#include <string.h>

const char *xhci_cc_name(uint8_t cc) {
    static const char *names[] = {
        [0]  = "INVALID",            [1]  = "SUCCESS",
        [2]  = "DATA_BUFFER_ERROR",  [3]  = "BABBLE",
        [4]  = "USB_TRANSACTION_ERROR", [5] = "TRB_ERROR",
        [6]  = "STALL",              [7]  = "RESOURCE_ERROR",
        [8]  = "BANDWIDTH_ERROR",    [9]  = "NO_SLOTS_AVAILABLE",
        [10] = "INVALID_STREAM_TYPE", [11] = "SLOT_NOT_ENABLED",
        [12] = "ENDPOINT_NOT_ENABLED", [13] = "SHORT_PACKET",
        [14] = "RING_UNDERRUN",      [15] = "RING_OVERRUN",
        [16] = "VF_EVENT_RING_FULL", [17] = "PARAMETER_ERROR",
        [18] = "BANDWIDTH_OVERRUN",  [19] = "CONTEXT_STATE_ERROR",
        [20] = "NO_PING_RESPONSE",   [21] = "EVENT_RING_FULL",
        [22] = "INCOMPATIBLE_DEVICE", [23] = "MISSED_SERVICE",
        [24] = "COMMAND_RING_STOPPED", [25] = "COMMAND_ABORTED",
        [26] = "STOPPED",            [27] = "STOPPED_LENGTH_INVALID",
        [28] = "STOPPED_SHORT_PACKET", [29] = "MAX_EXIT_LATENCY_TOO_LARGE",
        [31] = "ISOCH_BUFFER_OVERRUN", [32] = "EVENT_LOST",
        [33] = "UNDEFINED_ERROR",    [34] = "INVALID_STREAM_ID",
        [35] = "SECONDARY_BANDWIDTH_ERROR", [36] = "SPLIT_TRANSACTION_ERROR",
    };
    if (cc < sizeof(names) / sizeof(names[0]) && names[cc])
        return names[cc];
    return "VENDOR_OR_RESERVED";
}

const char *xhci_trb_name(uint8_t type) {
    switch (type) {
        case XHCI_TRB_NORMAL:         return "NORMAL";
        case XHCI_TRB_SETUP:          return "SETUP";
        case XHCI_TRB_DATA:           return "DATA";
        case XHCI_TRB_STATUS:         return "STATUS";
        case XHCI_TRB_LINK:           return "LINK";
        case XHCI_TRB_ENABLE_SLOT:    return "ENABLE_SLOT";
        case XHCI_TRB_DISABLE_SLOT:   return "DISABLE_SLOT";
        case XHCI_TRB_ADDRESS_DEVICE: return "ADDRESS_DEVICE";
        case XHCI_TRB_CONFIGURE_EP:   return "CONFIGURE_ENDPOINT";
        case XHCI_TRB_EVALUATE_CTX:   return "EVALUATE_CONTEXT";
        case XHCI_TRB_RESET_EP:       return "RESET_ENDPOINT";
        case XHCI_TRB_STOP_EP:        return "STOP_ENDPOINT";
        case XHCI_TRB_SET_TR_DEQUEUE: return "SET_TR_DEQUEUE";
        case XHCI_TRB_NOOP_CMD:       return "NOOP";
        case XHCI_TRB_EVT_TRANSFER:   return "TRANSFER_EVENT";
        case XHCI_TRB_EVT_COMMAND:    return "COMMAND_COMPLETION";
        case XHCI_TRB_EVT_PORT:       return "PORT_STATUS_CHANGE";
        case XHCI_TRB_EVT_HOST:       return "HOST_CONTROLLER_EVENT";
    }
    return "UNKNOWN";
}

// one segment is plenty, the controller writes and we chase it (4.9.4)
bool xhci_event_ring_init(xhci_hc_t *hc) {
    xhci_ring_t *ring = &hc->event_ring;

    ring->trbs = usb_dma_alloc(1, &ring->phys, !hc->ac64);
    hc->erst   = usb_dma_alloc(1, &hc->erst_phys, !hc->ac64);
    if (!ring->trbs || !hc->erst) {
        serial_printf(LOG_ERROR "xhci%d: OOM allocating the event ring\n", hc->index);
        return false;
    }
    ring->index = 0;
    ring->cycle = 1;

    hc->erst[0].base = ring->phys;
    hc->erst[0].size = XHCI_RING_TRBS;

    // ERSTBA has to be written last, thats what makes the controller read the table
    uint64_t ir = hc->rt + XHCI_RT_IR0;
    usb_mmio_write32(ir, XHCI_IR_ERSTSZ, 1);
    usb_mmio_write64(ir, XHCI_IR_ERDP, ring->phys);
    usb_mmio_write64(ir, XHCI_IR_ERSTBA, hc->erst_phys);

    serial_printf(LOG_INFO "xhci%d: event ring at 0x%llx, %u trbs, erst at 0x%llx\n",
                  hc->index, (uint64_t)ring->phys, XHCI_RING_TRBS, (uint64_t)hc->erst_phys);
    return true;
}

static void xhci_event_dispatch(xhci_hc_t *hc, const xhci_trb_t *event) {
    uint8_t type = XHCI_TRB_GET_TYPE(event->control);

    xhci_trace("xhci%d: event %s param=0x%llx status=0x%08x control=0x%08x\n",
               hc->index, xhci_trb_name(type), event->param, event->status, event->control);

    switch (type) {
        case XHCI_TRB_EVT_TRANSFER:
            xhci_transfer_event(hc, event);
            break;
        case XHCI_TRB_EVT_COMMAND:
            xhci_cmd_complete(hc, event);
            break;
        case XHCI_TRB_EVT_PORT:
            xhci_port_change(hc, event);
            break;
        case XHCI_TRB_EVT_HOST:
            serial_printf(LOG_ERROR "xhci%d: host controller event %s, usbsts=0x%08x\n",
                          hc->index, xhci_cc_name(XHCI_EVT_CC(event->status)),
                          xhci_op_read(hc, XHCI_OP_USBSTS));
            break;
        default:
            serial_printf(LOG_WARN "xhci%d: ignoring event type %u (%s)\n",
                          hc->index, type, xhci_trb_name(type));
            break;
    }
}

// safe from the timer tick and from anyone spinning on a transfer, the lock
// keeps them from both chasing the ring at once
void xhci_event_poll(xhci_hc_t *hc) {
    xhci_ring_t *ring = &hc->event_ring;
    bool         any  = false;

    unsigned long irq = irq_push();
    spinlock_acquire(&hc->event_lock);

    for (;;) {
        xhci_trb_t *event = &ring->trbs[ring->index];
        if ((event->control & XHCI_TRB_CYCLE) != ring->cycle)
            break;

        xhci_event_dispatch(hc, event);
        any = true;

        if (++ring->index == XHCI_RING_TRBS) {
            ring->index  = 0;
            ring->cycle ^= 1;
        }
    }

    // tell it how far we got, otherwise it thinks the ring is full eventually
    if (any) {
        paddr_t dequeue = ring->phys + ring->index * sizeof(xhci_trb_t);
        usb_mmio_write64(hc->rt + XHCI_RT_IR0, XHCI_IR_ERDP, dequeue | XHCI_ERDP_EHB);
    }

    spinlock_release(&hc->event_lock);
    irq_restore(irq);
}
