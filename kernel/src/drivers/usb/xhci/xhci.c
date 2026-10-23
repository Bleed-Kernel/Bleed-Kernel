#include "xhci_priv.h"
#include <drivers/usb/xhci/xhci.h>
#include <mm/paging.h>
#include <ACPI/acpi_hpet.h>
#include <string.h>

/*
    bring up order, every step logs so a serial dump from real hardware says where it died
      1. pci + bar, intel port routing
      2. extended capabilities, BIOS handoff and which ports are usb2/usb3
      3. halt, reset, read the parameters
      4. dcbaa, scratchpads, command ring, event ring
      5. run, prove the rings work with a NOOP, then walk the ports
*/

static xhci_hc_t xhci_controllers[XHCI_MAX_CONTROLLERS];
static int       xhci_count = 0;

// intel chipsets from around 2012-2015 put ehci and xhci on the same sockets and
// boot with ehci owning them. only touch these registers if theres an ehci to share with,
// on anything newer they mean something else
static void xhci_intel_route_ports(xhci_hc_t *hc) {
    pci_device_t *p = &hc->pci;
    pci_device_t  ehci;

    if (p->vendor_id != PCI_VENDOR_INTEL ||
        !pci_find_device(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_EHCI, &ehci))
        return;

    uint32_t usb3_mask = pci_read32(p->bus, p->device, p->function, XHCI_INTEL_USB3PRM);
    uint32_t usb2_mask = pci_read32(p->bus, p->device, p->function, XHCI_INTEL_XUSB2PRM);

    pci_write32(p->bus, p->device, p->function, XHCI_INTEL_USB3_PSSEN, usb3_mask);
    pci_write32(p->bus, p->device, p->function, XHCI_INTEL_XUSB2PR, usb2_mask);

    serial_printf(LOG_INFO "xhci%d: intel shared ports switched over, usb3 0x%x usb2 0x%x\n",
                  hc->index, usb3_mask, usb2_mask);
}

// USBLEGSUP (7.1), same dance as ehci but the capability lives in mmio this time
static void xhci_bios_handoff(xhci_hc_t *hc, uint64_t cap) {
    uint32_t legsup = usb_mmio_read32(cap, 0);
    serial_printf(LOG_INFO "xhci%d: legacy support cap, usblegsup=0x%08x\n", hc->index, legsup);

    if (legsup & XHCI_LEGSUP_BIOS) {
        usb_mmio_write32(cap, 0, legsup | XHCI_LEGSUP_OS);

        if (!usb_wait32(cap, 0, XHCI_LEGSUP_BIOS, false, XHCI_HANDOFF_TIMEOUT_MS)) {
            serial_printf(LOG_WARN "xhci%d: BIOS wont hand over, taking it anyway\n", hc->index);
            usb_mmio_write32(cap, 0, (usb_mmio_read32(cap, 0) & ~XHCI_LEGSUP_BIOS) | XHCI_LEGSUP_OS);
        } else {
            serial_printf(LOG_OK "xhci%d: BIOS handed the controller over\n", hc->index);
        }
    }

    // no more SMIs, the firmware doesnt get a say anymore
    uint32_t ctl = usb_mmio_read32(cap, 4);
    usb_mmio_write32(cap, 4, (ctl & XHCI_LEGCTL_KEEP) | XHCI_LEGCTL_SMI_EVENTS);
}

static void xhci_walk_caps(xhci_hc_t *hc, uint32_t hccparams1) {
    uint32_t offset = XHCI_HCC1_XECP(hccparams1) * 4;
    if (!offset) {
        serial_printf(LOG_WARN "xhci%d: no extended capabilities, assuming every port is usb2\n",
                      hc->index);
        memset(hc->port_major, 2, sizeof(hc->port_major));
        return;
    }

    for (int guard = 0; offset && guard < 64; guard++) {
        uint64_t cap  = hc->base + offset;
        uint32_t head = usb_mmio_read32(cap, 0);

        switch (XHCI_XCAP_ID(head)) {
            case XHCI_XCAP_LEGACY:
                xhci_bios_handoff(hc, cap);
                break;
            case XHCI_XCAP_PROTOCOL:
                xhci_port_protocol(hc, cap);
                break;
            default:
                xhci_trace("xhci%d: extended cap %u at 0x%x\n", hc->index, XHCI_XCAP_ID(head), offset);
                break;
        }

        uint32_t next = XHCI_XCAP_NEXT(head) * 4;
        offset = next ? offset + next : 0;
    }
}

static bool xhci_reset(xhci_hc_t *hc) {
    if (!(xhci_op_read(hc, XHCI_OP_USBSTS) & XHCI_STS_HCH)) {
        xhci_op_write(hc, XHCI_OP_USBCMD, xhci_op_read(hc, XHCI_OP_USBCMD) & ~XHCI_CMD_RUN);
        if (!usb_wait32(hc->op, XHCI_OP_USBSTS, XHCI_STS_HCH, true, XHCI_HALT_TIMEOUT_MS)) {
            serial_printf(LOG_ERROR "xhci%d: wont halt, usbsts=0x%08x\n",
                          hc->index, xhci_op_read(hc, XHCI_OP_USBSTS));
            return false;
        }
    }

    xhci_op_write(hc, XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    // some intel parts lock up if you read the registers straight after asking for a reset
    usb_delay_ms(1);

    if (!usb_wait32(hc->op, XHCI_OP_USBCMD, XHCI_CMD_HCRST, false, XHCI_RESET_TIMEOUT_MS) ||
        !usb_wait32(hc->op, XHCI_OP_USBSTS, XHCI_STS_CNR, false, XHCI_RESET_TIMEOUT_MS)) {
        serial_printf(LOG_ERROR "xhci%d: reset timed out, usbcmd=0x%08x usbsts=0x%08x\n", hc->index,
                      xhci_op_read(hc, XHCI_OP_USBCMD), xhci_op_read(hc, XHCI_OP_USBSTS));
        return false;
    }

    serial_printf(LOG_INFO "xhci%d: reset done\n", hc->index);
    return true;
}

static void xhci_read_params(xhci_hc_t *hc) {
    uint32_t hcs1 = usb_mmio_read32(hc->base, XHCI_CAP_HCSPARAMS1);
    uint32_t hcs2 = usb_mmio_read32(hc->base, XHCI_CAP_HCSPARAMS2);
    uint32_t hcc1 = usb_mmio_read32(hc->base, XHCI_CAP_HCCPARAMS1);
    uint32_t page = xhci_op_read(hc, XHCI_OP_PAGESIZE);

    hc->max_slots     = XHCI_HCS1_MAX_SLOTS(hcs1);
    hc->max_ports     = XHCI_HCS1_MAX_PORTS(hcs1);
    hc->scratch_count = XHCI_HCS2_SCRATCH(hcs2);
    hc->ac64          = hcc1 & XHCI_HCC1_AC64;
    hc->ctx_size      = (hcc1 & XHCI_HCC1_CSZ) ? 64 : 32;

    serial_printf(LOG_INFO "xhci%d: hcsparams1=0x%08x hcsparams2=0x%08x hccparams1=0x%08x\n",
                  hc->index, hcs1, hcs2, hcc1);
    serial_printf(LOG_INFO "xhci%d: %u slots, %u ports, %u interrupters, %u scratchpad pages\n",
                  hc->index, hc->max_slots, hc->max_ports, XHCI_HCS1_MAX_INTRS(hcs1),
                  hc->scratch_count);
    serial_printf(LOG_INFO "xhci%d: %s addressing, %u byte contexts, port power %s, pagesize 0x%x\n",
                  hc->index, hc->ac64 ? "64bit" : "32bit", hc->ctx_size,
                  (hcc1 & XHCI_HCC1_PPC) ? "switched" : "always on", page);

    if (!(page & 1))
        serial_printf(LOG_WARN "xhci%d: doesnt do 4K pages, this wont end well\n", hc->index);
    if (hc->version < 0x100)
        serial_printf(LOG_WARN "xhci%d: pre 1.0 controller, TD sizes will be off\n", hc->index);
}

// the controller gets a page of its own memory per scratchpad entry and never tells us what for
static bool xhci_scratchpads_init(xhci_hc_t *hc) {
    if (hc->scratch_count == 0)
        return true;
    if (hc->scratch_count > PAGE_SIZE / sizeof(uint64_t)) {
        serial_printf(LOG_ERROR "xhci%d: wants %u scratchpad pages, we can only list %u\n",
                      hc->index, hc->scratch_count, (unsigned)(PAGE_SIZE / sizeof(uint64_t)));
        return false;
    }

    paddr_t   array_phys;
    uint64_t *array = usb_dma_alloc(1, &array_phys, !hc->ac64);
    if (!array)
        return false;

    for (uint32_t i = 0; i < hc->scratch_count; i++) {
        paddr_t page;
        if (!usb_dma_alloc(1, &page, !hc->ac64)) {
            serial_printf(LOG_ERROR "xhci%d: OOM on scratchpad page %u\n", hc->index, i);
            return false;
        }
        array[i] = page;
    }

    hc->dcbaa[0] = array_phys;
    serial_printf(LOG_INFO "xhci%d: %u scratchpad pages, array at 0x%llx\n",
                  hc->index, hc->scratch_count, (uint64_t)array_phys);
    return true;
}

static bool xhci_start(xhci_hc_t *hc) {
    uint8_t slots = hc->max_slots < XHCI_MAX_SLOTS ? hc->max_slots : XHCI_MAX_SLOTS;
    xhci_op_write(hc, XHCI_OP_CONFIG, slots);

    hc->dcbaa = usb_dma_alloc(1, &hc->dcbaa_phys, !hc->ac64);
    if (!hc->dcbaa) {
        serial_printf(LOG_ERROR "xhci%d: OOM allocating the DCBAA\n", hc->index);
        return false;
    }
    if (!xhci_scratchpads_init(hc))
        return false;
    usb_mmio_write64(hc->op, XHCI_OP_DCBAAP, hc->dcbaa_phys);
    serial_printf(LOG_INFO "xhci%d: %u slots enabled, dcbaa at 0x%llx\n",
                  hc->index, slots, (uint64_t)hc->dcbaa_phys);

    if (!xhci_cmd_ring_init(hc) || !xhci_event_ring_init(hc))
        return false;

    // no interrupts, the timer tick drains the event ring
    xhci_op_write(hc, XHCI_OP_USBSTS, xhci_op_read(hc, XHCI_OP_USBSTS));
    xhci_op_write(hc, XHCI_OP_USBCMD, XHCI_CMD_RUN);
    if (!usb_wait32(hc->op, XHCI_OP_USBSTS, XHCI_STS_HCH, false, XHCI_HALT_TIMEOUT_MS)) {
        serial_printf(LOG_ERROR "xhci%d: wont start, usbsts=0x%08x\n",
                      hc->index, xhci_op_read(hc, XHCI_OP_USBSTS));
        return false;
    }

    uint32_t sts = xhci_op_read(hc, XHCI_OP_USBSTS);
    if (sts & (XHCI_STS_HSE | XHCI_STS_HCE)) {
        serial_printf(LOG_ERROR "xhci%d: errored out right after starting, usbsts=0x%08x\n",
                      hc->index, sts);
        return false;
    }

    // if this comes back the rings, the doorbells and the cycle bits all work
    if (xhci_cmd_noop(hc) != XHCI_CC_SUCCESS) {
        serial_printf(LOG_ERROR "xhci%d: NOOP command didnt come back, giving up on it\n", hc->index);
        return false;
    }

    serial_printf(LOG_OK "xhci%d: running\n", hc->index);
    return true;
}

static bool xhci_controller_init(xhci_hc_t *hc) {
    uint64_t bar_size = usb_bar0_size(&hc->pci);
    usb_pci_enable(&hc->pci);

    hc->base = usb_map_bar0(&hc->pci, bar_size ? bar_size : 0x10000);
    if (!hc->base) {
        serial_printf(LOG_ERROR "xhci%d: BAR0 is zero or wont map\n", hc->index);
        return false;
    }

    hc->version = (uint16_t)(usb_mmio_read32(hc->base, 0) >> 16);
    hc->op      = hc->base + (usb_mmio_read32(hc->base, XHCI_CAP_CAPLENGTH) & 0xFF);
    hc->rt      = hc->base + (usb_mmio_read32(hc->base, XHCI_CAP_RTSOFF) & ~0x1Fu);
    hc->db      = hc->base + (usb_mmio_read32(hc->base, XHCI_CAP_DBOFF) & ~0x3u);

    serial_printf(LOG_INFO "xhci%d: version %x.%02x, bar0 %llu KiB, op +0x%llx rt +0x%llx db +0x%llx\n",
                  hc->index, hc->version >> 8, hc->version & 0xFF, bar_size / 1024,
                  hc->op - hc->base, hc->rt - hc->base, hc->db - hc->base);

    xhci_intel_route_ports(hc);
    xhci_walk_caps(hc, usb_mmio_read32(hc->base, XHCI_CAP_HCCPARAMS1));

    if (!xhci_reset(hc))
        return false;
    xhci_read_params(hc);
    if (!xhci_start(hc))
        return false;

    hc->present = true;
    xhci_ports_probe(hc);
    hc->running = true;
    return true;
}

void xhci_hotplug(void) {
    for (int i = 0; i < xhci_count; i++) {
        if (xhci_controllers[i].running)
            xhci_ports_service(&xhci_controllers[i]);
    }
}

void xhci_poll(void) {
    for (int i = 0; i < xhci_count; i++) {
        if (xhci_controllers[i].running)
            xhci_event_poll(&xhci_controllers[i]);
    }
}

void xhci_init(void) {
    memset(xhci_controllers, 0, sizeof(xhci_controllers));

    pci_device_t pci_dev;
    for (int n = 0; xhci_count < XHCI_MAX_CONTROLLERS &&
                    pci_find_device_nth(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                        PCI_PROGIF_XHCI, n, &pci_dev); n++) {
        xhci_hc_t *hc = &xhci_controllers[xhci_count];
        hc->index = xhci_count;
        hc->pci   = pci_dev;
        spinlock_init(&hc->cmd_lock);
        spinlock_init(&hc->event_lock);

        serial_printf(LOG_OK "xhci%d: controller found - PCI " PCI_SPEC " vendor=0x%04x device=0x%04x\n",
                      hc->index, PCI_SPEC_ARGS(&pci_dev), pci_dev.vendor_id, pci_dev.device_id);

        if (xhci_controller_init(hc))
            xhci_count++;
        else
            serial_printf(LOG_ERROR "xhci%d: init failed, its devices wont work\n", hc->index);
    }

    if (xhci_count == 0)
        serial_printf(LOG_INFO "xhci: no xHCI controller found\n");
}
