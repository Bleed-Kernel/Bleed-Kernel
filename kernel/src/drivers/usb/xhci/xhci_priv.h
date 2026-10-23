#pragma once

// shared between the xhci_*.c files and nobody else, the rest of the kernel gets xhci.h

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/usb/xhci/xhci_regs.h>
#include <drivers/ahci/pci.h>
#include <drivers/serial/serial.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <ansii.h>
#include <stdio.h>

#define XHCI_MAX_CONTROLLERS    2
#define XHCI_MAX_SLOTS          32      // devices per controller we bother with
#define XHCI_MAX_DCI            32      // device context index, ep0 is 1 and 31 is ep15 in
#define XHCI_RING_TRBS          256     // a page worth, the last one is the link back
#define XHCI_XFER_MAX_TRBS      8       // biggest TD we queue, each trb moves up to 64K
#define XHCI_TRB_MAX_LEN        0x10000 // and cant cross a 64K boundary either

#define XHCI_HALT_TIMEOUT_MS    20
#define XHCI_RESET_TIMEOUT_MS   1000
#define XHCI_HANDOFF_TIMEOUT_MS 1000
#define XHCI_CMD_TIMEOUT_MS     500
#define XHCI_CTRL_TIMEOUT_MS    500
#define XHCI_BULK_TIMEOUT_MS    5000    // a cold usb stick can sit on a read for a while
#define XHCI_PORT_RESET_MS      100
#define XHCI_PORT_POWER_MS      20
#define XHCI_PORT_DEBOUNCE_MS   100     // usb2 spec wants 100ms between attach and reset
#define XHCI_PIPE_MAX_ERRORS    8

// not a real completion code, what a wait returns when the device got unplugged under it
#define XHCI_CC_GONE            0xFF

#define xhci_trace(...) do { if (XHCI_TRACE) serial_printf(LOG_INFO __VA_ARGS__); } while (0)

// a producer ring (command or transfer) or the event ring, index is our end of it
typedef struct xhci_ring {
    xhci_trb_t *trbs;
    paddr_t     phys;
    uint32_t    index;
    uint8_t     cycle;
} xhci_ring_t;

// one TD in flight on an endpoint, the event handler fills in the result
typedef struct xhci_xfer {
    paddr_t       trbs[XHCI_XFER_MAX_TRBS];
    uint32_t      lens[XHCI_XFER_MAX_TRBS];
    int           count;
    volatile bool done;
    uint8_t       cc;
    uint32_t      actual;
} xhci_xfer_t;

typedef struct xhci_ep {
    bool        used;
    bool        halted;         // the controller stopped it after an error, needs a reset
    uint8_t     dci;
    uint8_t     type;           // XHCI_EP_TYPE_*
    uint16_t    max_packet;
    xhci_ring_t ring;
    xhci_xfer_t xfer;

    // interrupt pipes only, the report lands in buf and goes to cb
    usb_interrupt_cb_t cb;
    void       *ctx;
    uint8_t    *buf;
    paddr_t     buf_phys;
    uint8_t     errors;
} xhci_ep_t;

struct xhci_hc;

typedef struct xhci_slot {
    struct xhci_hc *hc;
    uint8_t      id;
    uint8_t      port;          // root port, counted from 1 like the spec does
    uint8_t      speed;         // XHCI_SPEED_*
    uint8_t      context_entries;

    void        *in_ctx;        // what we hand the controller with each command
    paddr_t      in_ctx_phys;
    void        *out_ctx;       // what the controller writes back, the DCBAA points here
    paddr_t      out_ctx_phys;
    uint8_t     *ctrl_buf;      // ep0 data stage
    paddr_t      ctrl_buf_phys;

    xhci_ep_t    eps[XHCI_MAX_DCI];
    usb_device_t dev;
} xhci_slot_t;

typedef struct xhci_hc {
    bool         present;
    bool         running;       // probe is over, port changes now mean hotplug
    int          index;
    pci_device_t pci;

    uint64_t     base;          // capability registers
    uint64_t     op;
    uint64_t     rt;
    uint64_t     db;

    uint16_t     version;
    uint8_t      max_slots;
    uint8_t      max_ports;
    uint32_t     ctx_size;      // 32 or 64
    bool         ac64;
    uint32_t     scratch_count;
    uint8_t      port_major[256];   // usb 2 or 3 per port, from the protocol capabilities
    uint8_t      port_slot[256];    // whats plugged into each port, 0 for nothing
    volatile uint32_t ports_changed[256 / 32];  // set by the event handler, usb_service clears

    uint64_t    *dcbaa;
    paddr_t      dcbaa_phys;

    xhci_ring_t  cmd_ring;
    xhci_ring_t  event_ring;
    xhci_erst_entry_t *erst;
    paddr_t      erst_phys;

    // the one command in flight
    spinlock_t    cmd_lock;
    paddr_t       cmd_trb;
    volatile bool cmd_done;
    uint8_t       cmd_cc;
    uint8_t       cmd_slot;

    spinlock_t   event_lock;
    xhci_slot_t *slots[XHCI_MAX_SLOTS + 1];     // by slot id, 0 isnt a slot
} xhci_hc_t;

// register helpers
static inline uint32_t xhci_op_read(xhci_hc_t *hc, uint32_t reg) {
    return usb_mmio_read32(hc->op, reg);
}

static inline void xhci_op_write(xhci_hc_t *hc, uint32_t reg, uint32_t val) {
    usb_mmio_write32(hc->op, reg, val);
}

static inline void xhci_doorbell(xhci_hc_t *hc, uint8_t slot, uint8_t target) {
    usb_mmio_write32(hc->db, (uint32_t)slot * 4, target);
}

// endpoint address to device context index, ep0 is 1 and the rest go out then in
static inline uint8_t xhci_dci(uint8_t endpoint) {
    uint8_t num = endpoint & USB_ENDPOINT_NUM_MASK;
    if (num == 0)
        return 1;
    return (uint8_t)(num * 2 + ((endpoint & USB_ENDPOINT_IN) ? 1 : 0));
}

static inline bool xhci_cc_ok(uint8_t cc) {
    return cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET;
}

// contexts are 32 or 64 bytes depending on CSZ so they cant be a plain array.
// the input context has its control context in front, so index 1 is the slot there
static inline uint32_t *xhci_in_ctx(xhci_slot_t *slot, uint32_t index) {
    return (uint32_t *)((uint8_t *)slot->in_ctx + index * slot->hc->ctx_size);
}

static inline uint32_t *xhci_out_ctx(xhci_slot_t *slot, uint32_t index) {
    return (uint32_t *)((uint8_t *)slot->out_ctx + index * slot->hc->ctx_size);
}

// xhci_ring.c
bool    xhci_ring_init(xhci_hc_t *hc, xhci_ring_t *ring);
void    xhci_ring_free(xhci_ring_t *ring);
paddr_t xhci_ring_push(xhci_ring_t *ring, uint64_t param, uint32_t status, uint32_t control);
paddr_t xhci_ring_enqueue_ptr(const xhci_ring_t *ring);

// xhci_event.c
bool        xhci_event_ring_init(xhci_hc_t *hc);
void        xhci_event_poll(xhci_hc_t *hc);
const char *xhci_cc_name(uint8_t cc);
const char *xhci_trb_name(uint8_t type);

// xhci_cmd.c
bool    xhci_cmd_ring_init(xhci_hc_t *hc);
void    xhci_cmd_complete(xhci_hc_t *hc, const xhci_trb_t *event);
uint8_t xhci_cmd_noop(xhci_hc_t *hc);
uint8_t xhci_cmd_enable_slot(xhci_hc_t *hc, uint8_t *slot_id);
uint8_t xhci_cmd_disable_slot(xhci_hc_t *hc, uint8_t slot_id);
uint8_t xhci_cmd_address_device(xhci_hc_t *hc, xhci_slot_t *slot);
uint8_t xhci_cmd_configure_ep(xhci_hc_t *hc, xhci_slot_t *slot);
uint8_t xhci_cmd_evaluate_ctx(xhci_hc_t *hc, xhci_slot_t *slot);
uint8_t xhci_cmd_reset_ep(xhci_hc_t *hc, xhci_slot_t *slot, uint8_t dci);
uint8_t xhci_cmd_stop_ep(xhci_hc_t *hc, xhci_slot_t *slot, uint8_t dci);
uint8_t xhci_cmd_set_tr_dequeue(xhci_hc_t *hc, xhci_slot_t *slot, uint8_t dci, paddr_t dequeue);

// xhci_port.c
void        xhci_port_protocol(xhci_hc_t *hc, uint64_t cap);
void        xhci_ports_probe(xhci_hc_t *hc);
void        xhci_port_change(xhci_hc_t *hc, const xhci_trb_t *event);
usb_speed_t xhci_usb_speed(uint8_t speed);

// xhci_port.c hotplug, runs in usb_service
void        xhci_ports_service(xhci_hc_t *hc);

// xhci_device.c
void xhci_device_attach(xhci_hc_t *hc, uint8_t port, uint8_t speed);
void xhci_device_detach(xhci_hc_t *hc, xhci_slot_t *slot);
int  xhci_endpoint_add(xhci_slot_t *slot, const usb_endpoint_desc_t *desc, uint8_t burst);

// xhci_transfer.c
extern const usb_hc_ops_t xhci_ops;
int  xhci_control(usb_device_t *dev, const usb_setup_t *setup, void *data);
void xhci_transfer_event(xhci_hc_t *hc, const xhci_trb_t *event);
