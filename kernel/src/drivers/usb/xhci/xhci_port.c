#include "xhci_priv.h"
#include <ACPI/acpi_hpet.h>
#include <string.h>

usb_speed_t xhci_usb_speed(uint8_t speed) {
    switch (speed) {
        case XHCI_SPEED_LOW:  return USB_SPEED_LOW;
        case XHCI_SPEED_FULL: return USB_SPEED_FULL;
        case XHCI_SPEED_HIGH: return USB_SPEED_HIGH;
        default:              return USB_SPEED_SUPER;
    }
}

static uint32_t xhci_portsc(xhci_hc_t *hc, uint8_t port) {
    return xhci_op_read(hc, XHCI_OP_PORTSC(port));
}

// PORTSC cant be read-modify-written, PED and the change bits would get hit by
// the 1s we read back. keep the harmless bits and add only what we mean
static void xhci_portsc_write(xhci_hc_t *hc, uint8_t port, uint32_t set) {
    uint32_t portsc = xhci_portsc(hc, port);
    xhci_op_write(hc, XHCI_OP_PORTSC(port), (portsc & XHCI_PORTSC_KEEP) | set);
}

static void xhci_port_log(xhci_hc_t *hc, uint8_t port, const char *what) {
    uint32_t portsc = xhci_portsc(hc, port);
    serial_printf(LOG_INFO "xhci%d: port %u (usb%u) %s - portsc=0x%08x ccs=%u ped=%u pls=%u pp=%u speed=%u\n",
                  hc->index, port, hc->port_major[port], what, portsc,
                  !!(portsc & XHCI_PORTSC_CCS), !!(portsc & XHCI_PORTSC_PED),
                  XHCI_PORTSC_PLS(portsc), !!(portsc & XHCI_PORTSC_PP),
                  XHCI_PORTSC_SPEED(portsc));
}

// supported protocol capability (7.2), says which ports are usb2 and which are usb3.
// a usb3 socket shows up twice, once on each side, and a usb2 stick in it uses the usb2 half
void xhci_port_protocol(xhci_hc_t *hc, uint64_t cap) {
    uint32_t head  = usb_mmio_read32(cap, 0);
    uint32_t name  = usb_mmio_read32(cap, 4);
    uint32_t ports = usb_mmio_read32(cap, 8);

    uint8_t major = XHCI_PROTO_MAJOR(head);
    uint8_t first = XHCI_PROTO_PORT_OFF(ports);
    uint8_t count = XHCI_PROTO_PORT_CNT(ports);

    char label[5];
    memcpy(label, &name, 4);
    label[4] = '\0';

    serial_printf(LOG_INFO "xhci%d: protocol \"%s\" %x.%02x on ports %u-%u\n",
                  hc->index, label, major, XHCI_PROTO_MINOR(head), first, first + count - 1);

    for (uint32_t port = first; port < (uint32_t)first + count && port < 256; port++)
        hc->port_major[port] = major;
}

static bool xhci_port_wait_change(xhci_hc_t *hc, uint8_t port, uint32_t change) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        if (xhci_portsc(hc, port) & change)
            return true;
        if (usb_timed_out(start, spins, XHCI_PORT_RESET_MS))
            return false;
        asm volatile("pause");
    }
}

// usb2 ports sit disabled until we reset them, usb3 ones train the link themselves
// and only get a reset if that didnt work out (4.3.1)
static bool xhci_port_reset(xhci_hc_t *hc, uint8_t port) {
    bool usb3 = hc->port_major[port] == 3;

    if (usb3 && (xhci_portsc(hc, port) & XHCI_PORTSC_PED))
        return true;

    xhci_portsc_write(hc, port, XHCI_PORTSC_PR);
    if (!xhci_port_wait_change(hc, port, XHCI_PORTSC_PRC)) {
        xhci_port_log(hc, port, "reset timed out");

        if (!usb3)
            return false;

        // hot reset didnt bring the link up, a warm reset is the bigger hammer
        xhci_portsc_write(hc, port, XHCI_PORTSC_WPR);
        if (!xhci_port_wait_change(hc, port, XHCI_PORTSC_WRC)) {
            xhci_port_log(hc, port, "warm reset timed out");
            return false;
        }
    }
    xhci_portsc_write(hc, port, XHCI_PORTSC_CHANGE_MASK);

    // devices get 10ms to recover from a reset before you can talk to them
    usb_delay_ms(10);

    if (!(xhci_portsc(hc, port) & XHCI_PORTSC_PED)) {
        xhci_port_log(hc, port, "wont enable after reset");
        return false;
    }
    return true;
}

static void xhci_port_connect(xhci_hc_t *hc, uint8_t port) {
    xhci_port_log(hc, port, "connected");
    if (!xhci_port_reset(hc, port))
        return;
    xhci_port_log(hc, port, "enabled");

    xhci_device_attach(hc, port, XHCI_PORTSC_SPEED(xhci_portsc(hc, port)));
}

void xhci_ports_probe(xhci_hc_t *hc) {
    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        if (!(xhci_portsc(hc, port) & XHCI_PORTSC_PP))
            xhci_portsc_write(hc, port, XHCI_PORTSC_PP);
    }
    usb_delay_ms(XHCI_PORT_POWER_MS);

    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        uint32_t portsc = xhci_portsc(hc, port);
        if (!(portsc & XHCI_PORTSC_CCS)) {
            xhci_trace("xhci%d: port %u empty, portsc=0x%08x\n", hc->index, port, portsc);
            continue;
        }
        xhci_port_connect(hc, port);
    }
}

// event context, cant do anything that waits. note it and let usb_service deal with it
void xhci_port_change(xhci_hc_t *hc, const xhci_trb_t *event) {
    uint8_t port = XHCI_EVT_PORT(event->param);
    if (port == 0 || port > hc->max_ports)
        return;

    hc->ports_changed[port / 32] |= 1u << (port % 32);
    xhci_trace("xhci%d: port %u status change event\n", hc->index, port);
}

// compare what the port says with what we think is plugged into it. a connect
// change with something still attached means it got swapped out in under a poll
static void xhci_port_service(xhci_hc_t *hc, uint8_t port) {
    uint32_t portsc  = xhci_portsc(hc, port);
    uint32_t changes = portsc & XHCI_PORTSC_CHANGE_MASK;
    bool     present = portsc & XHCI_PORTSC_CCS;
    uint8_t  slot_id = hc->port_slot[port];

    if (changes)
        xhci_portsc_write(hc, port, changes);
    if (!(changes & XHCI_PORTSC_CSC) && (present || !slot_id))
        return;

    xhci_port_log(hc, port, present ? "plugged" : "unplugged");

    if (slot_id && hc->slots[slot_id])
        xhci_device_detach(hc, hc->slots[slot_id]);

    // usb2 says connected the moment a cable goes in, give the plug a moment to seat
    if (present) {
        usb_delay_ms(XHCI_PORT_DEBOUNCE_MS);
        if (xhci_portsc(hc, port) & XHCI_PORTSC_CCS)
            xhci_port_connect(hc, port);
        else
            xhci_port_log(hc, port, "gone again before it settled");
    }
}

void xhci_ports_service(xhci_hc_t *hc) {
    uint32_t changed[256 / 32];

    unsigned long irq = irq_push();
    for (int i = 0; i < 256 / 32; i++) {
        changed[i] = hc->ports_changed[i];
        hc->ports_changed[i] = 0;
    }
    irq_restore(irq);

    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        if (changed[port / 32] & (1u << (port % 32)))
            xhci_port_service(hc, (uint8_t)port);
    }
}
