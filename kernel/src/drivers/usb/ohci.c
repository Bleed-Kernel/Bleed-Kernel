#include <drivers/usb/ohci.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/ahci/pci.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <ACPI/acpi_hpet.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

#define MMIO(paddr)  ((uint64_t)(paddr) + g_gbi.hhdm_offset)

static ohci_hc_t ohci_controllers[OHCI_MAX_CONTROLLERS];
static int       ohci_count = 0;
static bool      ohci_ready = false;

// mmio helpers
static inline uint32_t ohci_read(uint64_t base, uint32_t offset) {
    return usb_mmio_read32(base, offset);
}

static inline void ohci_write(uint64_t base, uint32_t offset, uint32_t val) {
    usb_mmio_write32(base, offset, val);
}

// physical address of anything inside the controllers dma page
static inline uint32_t ohci_phys(ohci_hc_t *hc, const void *ptr) {
    return (uint32_t)(hc->dma_phys + ((uintptr_t)ptr - (uintptr_t)hc->dma));
}

static void ohci_td_fill(ohci_hc_t *hc, ohci_td_t *td, uint32_t flags,
                         const void *buf, uint16_t len, ohci_td_t *next) {
    uint32_t buf_phys = len ? ohci_phys(hc, buf) : 0;

    td->info = flags | OHCI_TD_CC_NOT_ACCESSED;
    td->cbp  = buf_phys;
    td->be   = len ? buf_phys + len - 1 : 0;
    td->next = ohci_phys(hc, next);
}

// queue the next IN on an interrupt endpoint.
// the td sitting at the tail is always an empty placeholder, fill that one in
// and make the old td the new placeholder, moving the tail is what hands it over
static void ohci_pipe_arm(ohci_hc_t *hc, ohci_pipe_t *pipe) {
    ohci_td_t *td    = pipe->tail;
    ohci_td_t *spare = pipe->td;

    ohci_td_fill(hc, td, OHCI_TD_DP_IN | OHCI_TD_ROUNDING | OHCI_TD_DI_NOW
                         | OHCI_TD_TOGGLE_ED, pipe->buf, pipe->len, spare);

    pipe->td   = td;
    pipe->tail = spare;
    pipe->ed->tail = ohci_phys(hc, spare);
}

static void ohci_pipe_complete(ohci_hc_t *hc, ohci_pipe_t *pipe, uint32_t cc) {
    if (cc == OHCI_CC_NOERROR) {
        uint32_t cbp = pipe->td->cbp;
        size_t   got = cbp ? cbp - ohci_phys(hc, pipe->buf) : pipe->len;

        pipe->errors = 0;
        pipe->cb(pipe->ctx, pipe->buf, got);
    } else if (++pipe->errors >= OHCI_PIPE_MAX_ERRORS) {
        // probably got unplugged, stop asking
        serial_printf(LOG_WARN "ohci: endpoint keeps failing (cc=%u), giving up on it\n", cc);
        pipe->ed->info |= OHCI_ED_SKIP;
        pipe->used = false;
        return;
    }

    ohci_pipe_arm(hc, pipe);

    // an error halts the endpoint, unhalt it but leave the toggle carry alone
    if (cc != OHCI_CC_NOERROR)
        pipe->ed->head &= ~OHCI_ED_HEAD_HALTED;
}

static void ohci_td_retired(ohci_hc_t *hc, uint32_t index) {
    ohci_td_t *td = &hc->dma->tds[index];
    uint32_t   cc = td->info >> OHCI_TD_CC_SHIFT;

    if (index < OHCI_TD_PIPE_BASE) {
        hc->ctrl_retired |= 1u << index;
        if (cc != OHCI_CC_NOERROR && !hc->ctrl_cc)
            hc->ctrl_cc = cc;
        return;
    }

    ohci_pipe_t *pipe = &hc->pipes[(index - OHCI_TD_PIPE_BASE) / 2];
    if (!pipe->used || td != pipe->td)
        return;
    ohci_pipe_complete(hc, pipe, cc);
}

// the controller chains every finished td onto a list and posts the head into the HCCA
static void ohci_collect_done_locked(ohci_hc_t *hc) {
    ohci_dma_t *dma  = hc->dma;
    uint32_t    done = dma->hcca.done_head & OHCI_PTR_MASK;
    if (!done)
        return;

    // it wont post another batch until we ack this one
    dma->hcca.done_head = 0;
    ohci_write(hc->base, OHCI_REG_INT_STATUS, OHCI_INT_WDH);

    uint32_t td_base = ohci_phys(hc, dma->tds);
    for (int guard = 0; done && guard < OHCI_TD_COUNT; guard++) {
        uint32_t index = (done - td_base) / sizeof(ohci_td_t);
        if (done < td_base || index >= OHCI_TD_COUNT) {
            serial_printf(LOG_ERROR "ohci: done queue has a td that isnt ours (0x%08x)\n", done);
            return;
        }

        // grab the link first, retiring an interrupt td rewrites it
        done = dma->tds[index].next & OHCI_PTR_MASK;
        ohci_td_retired(hc, index);
    }
}

static void ohci_collect_done(ohci_hc_t *hc) {
    unsigned long irq = irq_push();
    spinlock_acquire(&hc->lock);
    ohci_collect_done_locked(hc);
    spinlock_release(&hc->lock);
    irq_restore(irq);
}

static int ohci_control_wait(ohci_hc_t *hc, int last_td) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        ohci_collect_done(hc);

        if (hc->ctrl_cc)
            return -1;
        if (hc->ctrl_retired & (1u << last_td))
            return 0;
        if (usb_timed_out(start, spins, OHCI_CTRL_TIMEOUT_MS))
            return -2;
        asm volatile("pause");
    }
}

// setup -> (data) -> status, all on endpoint 0.
// not safe against itself, only ever called while probing with interrupts off
static int ohci_control(usb_device_t *dev, const usb_setup_t *setup, void *data) {
    if (!dev || !dev->hc || !setup) return USB_ERR;

    ohci_hc_t  *hc  = dev->hc;
    ohci_dma_t *dma = hc->dma;
    ohci_ed_t  *ed  = &dma->eds[OHCI_ED_CONTROL];
    ohci_td_t  *td  = dma->tds;

    uint16_t len = setup->length;
    bool     in  = setup->request_type & USB_REQ_DIR_IN;

    if (len > sizeof(dma->ctrl_buf) || (len && !data)) return USB_ERR;

    memcpy(dma->setup, setup, sizeof(*setup));
    if (len && !in)
        memcpy(dma->ctrl_buf, data, len);

    // setup is always DATA0 and the stages after it always start on DATA1
    ohci_td_fill(hc, &td[OHCI_TD_SETUP],
                 OHCI_TD_DP_SETUP | OHCI_TD_TOGGLE_DATA0 | OHCI_TD_DI_NONE,
                 dma->setup, sizeof(*setup),
                 len ? &td[OHCI_TD_DATA] : &td[OHCI_TD_STATUS]);

    if (len)
        ohci_td_fill(hc, &td[OHCI_TD_DATA],
                     (in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT) | OHCI_TD_ROUNDING
                     | OHCI_TD_TOGGLE_DATA1 | OHCI_TD_DI_NONE,
                     dma->ctrl_buf, len, &td[OHCI_TD_STATUS]);

    // status is an empty packet going the opposite way to the data
    ohci_td_fill(hc, &td[OHCI_TD_STATUS],
                 ((in && len) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN)
                 | OHCI_TD_TOGGLE_DATA1 | OHCI_TD_DI_NOW,
                 NULL, 0, &td[OHCI_TD_CTRL_TAIL]);

    hc->ctrl_retired = 0;
    hc->ctrl_cc      = 0;

    // the ed is parked on skip between transfers so its ours to rewrite
    ed->head = ohci_phys(hc, &td[OHCI_TD_SETUP]);
    ed->tail = ohci_phys(hc, &td[OHCI_TD_CTRL_TAIL]);
    ed->info = OHCI_ED_FA(dev->address) | OHCI_ED_EN(0) | OHCI_ED_DIR_TD
             | (dev->speed == USB_SPEED_LOW ? OHCI_ED_LOWSPEED : 0)
             | OHCI_ED_MPS(dev->max_packet0);

    ohci_write(hc->base, OHCI_REG_CMDSTATUS, OHCI_CMD_CLF);

    int r = ohci_control_wait(hc, OHCI_TD_STATUS);

    ed->info |= OHCI_ED_SKIP;
    if (r < 0) {
        // the controller might still be chewing on the ed, give it a frame
        // to notice the skip then sweep up whatever it retired
        usb_delay_ms(2);
        ohci_collect_done(hc);
        return r;
    }

    if (!len)
        return 0;

    uint32_t cbp = td[OHCI_TD_DATA].cbp;
    int      got = cbp ? (int)(cbp - ohci_phys(hc, dma->ctrl_buf)) : len;
    if (in)
        memcpy(data, dma->ctrl_buf, got);
    return got;
}

static int ohci_interrupt_in(usb_device_t *dev, const usb_endpoint_desc_t *ep,
                             usb_interrupt_cb_t cb, void *ctx) {
    if (!dev || !dev->hc || !cb) return USB_ERR;
    ohci_hc_t *hc         = dev->hc;
    uint8_t    endpoint   = ep->address;
    uint16_t   max_packet = ep->max_packet & USB_ENDPOINT_MPS_MASK;

    for (int i = 0; i < OHCI_MAX_PIPES; i++) {
        ohci_pipe_t *pipe = &hc->pipes[i];
        if (pipe->used) continue;

        pipe->dev    = dev;
        pipe->ed     = &hc->dma->eds[OHCI_ED_PIPE_BASE + i];
        pipe->td     = &hc->dma->tds[OHCI_TD_PIPE_BASE + i * 2];
        pipe->tail   = &hc->dma->tds[OHCI_TD_PIPE_BASE + i * 2 + 1];
        pipe->buf    = hc->dma->pipe_buf[i];
        pipe->len    = max_packet > OHCI_PIPE_BUF_SIZE ? OHCI_PIPE_BUF_SIZE : max_packet;
        pipe->errors = 0;
        pipe->cb     = cb;
        pipe->ctx    = ctx;
        pipe->used   = true;

        // head == tail means empty, the info write is what takes the skip off
        pipe->ed->head = ohci_phys(hc, pipe->tail);
        pipe->ed->tail = ohci_phys(hc, pipe->tail);
        pipe->ed->info = OHCI_ED_FA(dev->address)
                       | OHCI_ED_EN(endpoint & USB_ENDPOINT_NUM_MASK)
                       | OHCI_ED_DIR_IN
                       | (dev->speed == USB_SPEED_LOW ? OHCI_ED_LOWSPEED : 0)
                       | OHCI_ED_MPS(max_packet);

        ohci_pipe_arm(hc, pipe);
        return 0;
    }

    serial_printf(LOG_WARN "ohci: out of interrupt pipes\n");
    return USB_ERR;
}

// no bulk list yet, mass storage on these controllers logs that and moves on
static const usb_hc_ops_t ohci_ops = {
    .name         = "ohci",
    .control      = ohci_control,
    .interrupt_in = ohci_interrupt_in,
};

static bool ohci_port_reset(ohci_hc_t *hc, uint8_t port, bool *low_speed) {
    uint32_t reg = OHCI_REG_RH_PORT + (uint32_t)port * 4;

    if (!(ohci_read(hc->base, reg) & OHCI_PORT_CCS))
        return false;

    ohci_write(hc->base, reg, OHCI_PORT_PRS);
    if (!usb_wait32(hc->base, reg, OHCI_PORT_PRSC, true, OHCI_PORT_RESET_MS)) {
        serial_printf(LOG_WARN "ohci: port %u reset timeout\n", port);
        return false;
    }
    ohci_write(hc->base, reg, OHCI_PORT_CHANGE_MASK);

    // devices get 10ms to recover from a reset before you can talk to them
    usb_delay_ms(10);

    uint32_t status = ohci_read(hc->base, reg);
    if (!(status & OHCI_PORT_PES)) {
        ohci_write(hc->base, reg, OHCI_PORT_PES);
        status = ohci_read(hc->base, reg);
    }
    if (!(status & OHCI_PORT_PES)) {
        serial_printf(LOG_WARN "ohci: port %u wont enable\n", port);
        return false;
    }

    *low_speed = !!(status & OHCI_PORT_LSDA);
    return true;
}

// lowest address nobody on this controller has, 0 if all 127 are taken
static uint8_t ohci_free_address(ohci_hc_t *hc) {
    for (uint8_t address = 1; address <= 127; address++) {
        bool taken = false;
        for (int i = 0; i < OHCI_MAX_DEVICES; i++) {
            if (hc->devices[i].ops && hc->devices[i].address == address)
                taken = true;
        }
        if (!taken)
            return address;
    }
    return 0;
}

static usb_device_t *ohci_free_device(ohci_hc_t *hc) {
    for (int i = 0; i < OHCI_MAX_DEVICES; i++) {
        if (!hc->devices[i].ops)
            return &hc->devices[i];
    }
    return NULL;
}

static void ohci_port_enumerate(ohci_hc_t *hc, uint8_t port) {
    bool low_speed = false;
    if (!ohci_port_reset(hc, port, &low_speed))
        return;

    usb_device_t *dev     = ohci_free_device(hc);
    uint8_t       address = ohci_free_address(hc);
    if (!dev || !address) {
        serial_printf(LOG_WARN "ohci: port %u ignored, device table is full\n", port);
        return;
    }

    // fresh out of reset it answers on address 0
    memset(dev, 0, sizeof(*dev));
    dev->ops         = &ohci_ops;
    dev->hc          = hc;
    dev->port        = port;
    dev->speed       = low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev->max_packet0 = 8;

    // only the first 8 bytes are safe to ask for until we know how big ep0 packets are
    usb_device_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, &desc, 8) < 8) {
        serial_printf(LOG_WARN "ohci: port %u device isnt answering\n", port);
        memset(dev, 0, sizeof(*dev));
        return;
    }
    if (desc.max_packet0 >= 8)
        dev->max_packet0 = desc.max_packet0;

    if (usb_request(dev, USB_REQ_SET_ADDRESS, address) < 0) {
        serial_printf(LOG_WARN "ohci: port %u SET_ADDRESS failed\n", port);
        memset(dev, 0, sizeof(*dev));
        return;
    }
    usb_delay_ms(2);   // and 2ms to start listening on the new one
    dev->address = address;

    if (!usb_device_configure(dev)) {
        memset(dev, 0, sizeof(*dev));
        return;
    }
    hc->port_dev[port] = dev;
}

static void ohci_port_detach(ohci_hc_t *hc, uint8_t port) {
    usb_device_t *dev = hc->port_dev[port];
    hc->port_dev[port] = NULL;

    usb_device_disconnect(dev);

    for (int i = 0; i < OHCI_MAX_PIPES; i++) {
        if (hc->pipes[i].used && hc->pipes[i].dev == dev)
            hc->pipes[i].ed->info |= OHCI_ED_SKIP;
    }
    usb_delay_ms(2);

    unsigned long irq = irq_push();
    spinlock_acquire(&hc->lock);
    for (int i = 0; i < OHCI_MAX_PIPES; i++) {
        if (hc->pipes[i].used && hc->pipes[i].dev == dev) {
            hc->pipes[i].used = false;
            hc->pipes[i].dev  = NULL;
        }
    }
    memset(dev, 0, sizeof(*dev));
    spinlock_release(&hc->lock);
    irq_restore(irq);
}

// root hub ports latch a connect change, thats all we need to spot a plug or an unplug
static void ohci_port_service(ohci_hc_t *hc, uint8_t port) {
    uint32_t reg    = OHCI_REG_RH_PORT + (uint32_t)port * 4;
    uint32_t status = ohci_read(hc->base, reg);
    if (!(status & OHCI_PORT_CSC))
        return;
    ohci_write(hc->base, reg, OHCI_PORT_CHANGE_MASK);

    bool present = status & OHCI_PORT_CCS;
    serial_printf(LOG_INFO "ohci: port %u %s, status=0x%08x\n",
                  port, present ? "plugged" : "unplugged", status);

    if (hc->port_dev[port])
        ohci_port_detach(hc, port);

    if (present) {
        usb_delay_ms(OHCI_DEBOUNCE_MS);
        if (ohci_read(hc->base, reg) & OHCI_PORT_CCS)
            ohci_port_enumerate(hc, port);
        else
            serial_printf(LOG_INFO "ohci: port %u gone again before it settled\n", port);
    }
}

// if SMM/BIOS is driving the controller (legacy keyboard emulation) ask for it nicely
static void ohci_take_ownership(uint64_t base) {
    uint32_t control = ohci_read(base, OHCI_REG_CONTROL);
    if (!(control & OHCI_CTRL_IR))
        return;

    ohci_write(base, OHCI_REG_INT_ENABLE, OHCI_INT_OC);
    ohci_write(base, OHCI_REG_CMDSTATUS, OHCI_CMD_OCR);

    if (!usb_wait32(base, OHCI_REG_CONTROL, OHCI_CTRL_IR, false, OHCI_HANDOFF_TIMEOUT_MS)) {
        serial_printf(LOG_WARN "ohci: firmware wont hand over, taking it anyway\n");
        ohci_write(base, OHCI_REG_CONTROL, control & ~OHCI_CTRL_IR);
    }
}

static bool ohci_controller_init(ohci_hc_t *hc, const pci_device_t *pci_dev) {
    usb_pci_enable(pci_dev);

    uint64_t base = usb_map_bar0(pci_dev, PAGE_SIZE_4K);
    if (!base) {
        serial_printf(LOG_ERROR "ohci: BAR0 is zero or wont map\n");
        return false;
    }

    paddr_t dma_phys = pmm_alloc_pages(1);
    if (!dma_phys) {
        serial_printf(LOG_ERROR "ohci: OOM during controller init\n");
        return false;
    }
    // every pointer the controller follows is 32 bits wide
    if ((uint64_t)dma_phys >> 32) {
        serial_printf(LOG_ERROR "ohci: dma page landed above 4G (0x%llx)\n",
                      (uint64_t)dma_phys);
        pmm_free_pages(dma_phys, 1);
        return false;
    }

    memset(hc, 0, sizeof(*hc));
    hc->base         = base;
    hc->dma_phys     = dma_phys;
    hc->dma          = (ohci_dma_t *)(uintptr_t)MMIO(dma_phys);
    spinlock_init(&hc->lock);
    memset(hc->dma, 0, sizeof(*hc->dma));

    ohci_dma_t *dma = hc->dma;

    // every ed starts out skipped, opening a pipe is just filling one in.
    // the interrupt ones are chained up front so the list never gets relinked under the controller
    for (int i = 0; i < OHCI_ED_COUNT; i++)
        dma->eds[i].info = OHCI_ED_SKIP;
    for (int i = OHCI_ED_PIPE_BASE; i < OHCI_ED_COUNT - 1; i++)
        dma->eds[i].next = ohci_phys(hc, &dma->eds[i + 1]);

    // the table is indexed by frame number & 31, so every 8th slot = every 8ms
    for (int i = 0; i < 32; i += OHCI_POLL_FRAMES)
        dma->hcca.int_table[i] = ohci_phys(hc, &dma->eds[OHCI_ED_PIPE_BASE]);

    ohci_take_ownership(base);
    ohci_write(base, OHCI_REG_INT_DISABLE, 0xFFFFFFFF);

    // a reset wipes the frame interval so hang onto whatever the firmware tuned it to
    uint32_t fi = ohci_read(base, OHCI_REG_FM_INTERVAL) & OHCI_FM_INTERVAL_MASK;
    if (fi == 0)
        fi = OHCI_FM_INTERVAL_DEFAULT;

    ohci_write(base, OHCI_REG_CMDSTATUS, OHCI_CMD_HCR);
    if (!usb_wait32(base, OHCI_REG_CMDSTATUS, OHCI_CMD_HCR, false, OHCI_RESET_TIMEOUT_MS)) {
        serial_printf(LOG_ERROR "ohci: controller reset timeout\n");
        pmm_free_pages(dma_phys, 1);
        return false;
    }

    // reset leaves it suspended, if we arent operational within 2ms the
    // devices downstream suspend too so no logging until its running
    ohci_write(base, OHCI_REG_HCCA,         ohci_phys(hc, &dma->hcca));
    ohci_write(base, OHCI_REG_CONTROL_HEAD, ohci_phys(hc, &dma->eds[OHCI_ED_CONTROL]));
    ohci_write(base, OHCI_REG_CONTROL_CUR,  0);
    ohci_write(base, OHCI_REG_BULK_HEAD,    0);
    ohci_write(base, OHCI_REG_BULK_CUR,     0);

    // largest packet that still fits in whats left of a frame, formula is from the spec
    uint32_t fsmps = ((fi - 210) * 6) / 7;
    uint32_t fit   = (ohci_read(base, OHCI_REG_FM_INTERVAL) ^ OHCI_FM_FIT) & OHCI_FM_FIT;
    ohci_write(base, OHCI_REG_FM_INTERVAL,    fit | (fsmps << 16) | fi);
    ohci_write(base, OHCI_REG_PERIODIC_START, (fi * 9) / 10);

    ohci_write(base, OHCI_REG_CONTROL, OHCI_CTRL_HCFS_OPER | OHCI_CTRL_CLE
                                       | OHCI_CTRL_PLE | OHCI_CTRL_CBSR_4_1);
    ohci_write(base, OHCI_REG_INT_STATUS, 0x7FFFFFFF);

    // power the ports, harmless if they are always powered
    uint32_t rha = ohci_read(base, OHCI_REG_RH_DESC_A);
    hc->port_count = rha & OHCI_RHA_NDP_MASK;
    if (hc->port_count > OHCI_MAX_PORTS)
        hc->port_count = OHCI_MAX_PORTS;

    ohci_write(base, OHCI_REG_RH_STATUS, OHCI_RHS_LPSC);
    for (uint8_t port = 0; port < hc->port_count; port++)
        ohci_write(base, OHCI_REG_RH_PORT + (uint32_t)port * 4, OHCI_PORT_PPS);

    // power good time plus the 100ms a device gets to settle after attach
    usb_delay_ms((rha >> OHCI_RHA_POTPGT_SHIFT) * 2 + 100);

    hc->present = true;
    serial_printf(LOG_INFO "ohci: rev %x.%x, %u ports\n",
                  (ohci_read(base, OHCI_REG_REVISION) >> 4) & 0xF,
                  ohci_read(base, OHCI_REG_REVISION) & 0xF, hc->port_count);

    for (uint8_t port = 0; port < hc->port_count; port++) {
        serial_printf(LOG_INFO "ohci: port %u status 0x%08x\n", port,
                      ohci_read(base, OHCI_REG_RH_PORT + (uint32_t)port * 4));
        ohci_port_enumerate(hc, port);
    }

    return true;
}

void ohci_poll(void) {
    if (!ohci_ready) return;

    for (int i = 0; i < ohci_count; i++) {
        if (ohci_controllers[i].present)
            ohci_collect_done(&ohci_controllers[i]);
    }
}

void ohci_hotplug(void) {
    if (!ohci_ready) return;

    for (int i = 0; i < ohci_count; i++) {
        ohci_hc_t *hc = &ohci_controllers[i];
        if (!hc->present) continue;
        for (uint8_t port = 0; port < hc->port_count; port++)
            ohci_port_service(hc, port);
    }
}

void ohci_init(void) {
    memset(ohci_controllers, 0, sizeof(ohci_controllers));

    pci_device_t pci_dev;
    for (int n = 0; ohci_count < OHCI_MAX_CONTROLLERS &&
                    pci_find_device_nth(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                        PCI_PROGIF_OHCI, n, &pci_dev); n++) {
        serial_printf(LOG_OK "ohci: controller found - PCI %02x:%02x.%x "
                      "vendor=0x%04x device=0x%04x\n",
                      pci_dev.bus, pci_dev.device, pci_dev.function,
                      pci_dev.vendor_id, pci_dev.device_id);

        if (ohci_controller_init(&ohci_controllers[ohci_count], &pci_dev))
            ohci_count++;
    }

    if (ohci_count == 0)
        serial_printf(LOG_INFO "ohci: no OHCI controller found\n");

    ohci_ready = true;
}
