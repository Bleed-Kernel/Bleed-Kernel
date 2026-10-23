#include "xhci_priv.h"
#include <ACPI/acpi_hpet.h>
#include <string.h>

bool xhci_cmd_ring_init(xhci_hc_t *hc) {
    if (!xhci_ring_init(hc, &hc->cmd_ring)) {
        serial_printf(LOG_ERROR "xhci%d: OOM allocating the command ring\n", hc->index);
        return false;
    }

    usb_mmio_write64(hc->op, XHCI_OP_CRCR, hc->cmd_ring.phys | XHCI_CRCR_RCS);
    serial_printf(LOG_INFO "xhci%d: command ring at 0x%llx\n",
                  hc->index, (uint64_t)hc->cmd_ring.phys);
    return true;
}

void xhci_cmd_complete(xhci_hc_t *hc, const xhci_trb_t *event) {
    if ((event->param & ~0xFull) != hc->cmd_trb || hc->cmd_done) {
        serial_printf(LOG_WARN "xhci%d: completion for a command we werent waiting on (0x%llx)\n",
                      hc->index, event->param);
        return;
    }

    hc->cmd_cc   = XHCI_EVT_CC(event->status);
    hc->cmd_slot = XHCI_TRB_GET_SLOT(event->control);
    hc->cmd_done = true;
}

// a command that never finishes leaves the ring stuck, abort it so the next one can run
static void xhci_cmd_abort(xhci_hc_t *hc) {
    usb_mmio_write64(hc->op, XHCI_OP_CRCR, XHCI_CRCR_CA);
    if (!usb_wait32(hc->op, XHCI_OP_CRCR, XHCI_CRCR_CRR, false, XHCI_CMD_TIMEOUT_MS))
        serial_printf(LOG_ERROR "xhci%d: command ring wont stop, its probably dead\n", hc->index);
}

// queue one command and spin until its completion event comes back, returns the completion code
static uint8_t xhci_cmd(xhci_hc_t *hc, uint64_t param, uint32_t control, uint8_t *slot_out) {
    uint8_t type = XHCI_TRB_GET_TYPE(control);

    spinlock_acquire(&hc->cmd_lock);

    hc->cmd_done = false;
    hc->cmd_trb  = xhci_ring_push(&hc->cmd_ring, param, 0, control);
    xhci_doorbell(hc, 0, 0);

    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; !hc->cmd_done; spins++) {
        xhci_event_poll(hc);
        if (hc->cmd_done)
            break;
        if (usb_timed_out(start, spins, XHCI_CMD_TIMEOUT_MS)) {
            serial_printf(LOG_ERROR "xhci%d: %s timed out, usbsts=0x%08x crcr=0x%08x\n",
                          hc->index, xhci_trb_name(type), xhci_op_read(hc, XHCI_OP_USBSTS),
                          xhci_op_read(hc, XHCI_OP_CRCR));
            xhci_cmd_abort(hc);
            spinlock_release(&hc->cmd_lock);
            return XHCI_CC_INVALID;
        }
        asm volatile("pause");
    }

    uint8_t cc   = hc->cmd_cc;
    uint8_t slot = XHCI_TRB_GET_SLOT(control) ? XHCI_TRB_GET_SLOT(control) : hc->cmd_slot;
    if (slot_out)
        *slot_out = hc->cmd_slot;

    if (cc == XHCI_CC_SUCCESS)
        serial_printf(LOG_INFO "xhci%d: %s slot %u -> %s\n", hc->index,
                      xhci_trb_name(type), slot, xhci_cc_name(cc));
    else
        serial_printf(LOG_WARN "xhci%d: %s slot %u -> %s (%u)\n", hc->index,
                      xhci_trb_name(type), slot, xhci_cc_name(cc), cc);

    spinlock_release(&hc->cmd_lock);
    return cc;
}

uint8_t xhci_cmd_noop(xhci_hc_t *hc) {
    return xhci_cmd(hc, 0, XHCI_TRB_TYPE(XHCI_TRB_NOOP_CMD), NULL);
}

uint8_t xhci_cmd_enable_slot(xhci_hc_t *hc, uint8_t *slot_id) {
    return xhci_cmd(hc, 0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), slot_id);
}

uint8_t xhci_cmd_disable_slot(xhci_hc_t *hc, uint8_t slot_id) {
    return xhci_cmd(hc, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | XHCI_TRB_SLOT(slot_id), NULL);
}

uint8_t xhci_cmd_address_device(xhci_hc_t *hc, xhci_slot_t *slot) {
    return xhci_cmd(hc, slot->in_ctx_phys,
                    XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | XHCI_TRB_SLOT(slot->id), NULL);
}

uint8_t xhci_cmd_configure_ep(xhci_hc_t *hc, xhci_slot_t *slot) {
    return xhci_cmd(hc, slot->in_ctx_phys,
                    XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_EP) | XHCI_TRB_SLOT(slot->id), NULL);
}

uint8_t xhci_cmd_evaluate_ctx(xhci_hc_t *hc, xhci_slot_t *slot) {
    return xhci_cmd(hc, slot->in_ctx_phys,
                    XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CTX) | XHCI_TRB_SLOT(slot->id), NULL);
}

uint8_t xhci_cmd_reset_ep(xhci_hc_t *hc, xhci_slot_t *slot, uint8_t dci) {
    return xhci_cmd(hc, 0, XHCI_TRB_TYPE(XHCI_TRB_RESET_EP)
                         | XHCI_TRB_SLOT(slot->id) | XHCI_TRB_EP(dci), NULL);
}

uint8_t xhci_cmd_stop_ep(xhci_hc_t *hc, xhci_slot_t *slot, uint8_t dci) {
    return xhci_cmd(hc, 0, XHCI_TRB_TYPE(XHCI_TRB_STOP_EP)
                         | XHCI_TRB_SLOT(slot->id) | XHCI_TRB_EP(dci), NULL);
}

uint8_t xhci_cmd_set_tr_dequeue(xhci_hc_t *hc, xhci_slot_t *slot, uint8_t dci, paddr_t dequeue) {
    return xhci_cmd(hc, dequeue, XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQUEUE)
                               | XHCI_TRB_SLOT(slot->id) | XHCI_TRB_EP(dci), NULL);
}
