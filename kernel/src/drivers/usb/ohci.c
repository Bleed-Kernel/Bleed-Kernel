#include <drivers/usb/ohci.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb_keyboard.h>
#include <drivers/usb/usb_mouse.h>
#include <drivers/ahci/pci.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <cpu/control_registers.h>
#include <ACPI/acpi_hpet.h>

// EHCI bits, only enough to make it let go of the ports
#define EHCI_CAP_LENGTH     0x00
#define EHCI_CAP_HCCPARAMS  0x08
#define EHCI_OP_USBCMD      0x00
#define EHCI_OP_USBSTS      0x04
#define EHCI_OP_USBINTR     0x08
#define EHCI_OP_CONFIGFLAG  0x40

#define EHCI_CMD_RUN        (1u << 0)
#define EHCI_CMD_RESET      (1u << 1)
#define EHCI_STS_HALTED     (1u << 12)

#define EHCI_LEGSUP_CAPID   0x01
#define EHCI_LEGSUP_BIOS    (1u << 16)
#define EHCI_LEGSUP_OS      (1u << 24)

#define MMIO(paddr)  ((uint64_t)(paddr) + g_gbi.hhdm_offset)

static ohci_hc_t ohci_controllers[OHCI_MAX_CONTROLLERS];
static int       ohci_count = 0;
static bool      ohci_ready = false;

// mmio helpers
static inline uint32_t ohci_read(uint64_t base, uint32_t offset) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(base + offset);
    return *r;
}

static inline void ohci_write(uint64_t base, uint32_t offset, uint32_t val) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(base + offset);
    *r = val;
}

// physical address of anything inside the controllers dma page
static inline uint32_t ohci_phys(ohci_hc_t *hc, const void *ptr) {
    return (uint32_t)(hc->dma_phys + ((uintptr_t)ptr - (uintptr_t)hc->dma));
}

// has timeout_ms passed since start, timed off the HPET the same way ahci does it.
// wait_ms() is no good here, it yields to a scheduler that doesnt exist yet
static bool ohci_timed_out(uint64_t start, uint64_t spins, uint64_t timeout_ms) {
    uint64_t now = hpet_get_femtoseconds();
    if (now)
        return now - start > timeout_ms * femtosecondsPerMillisecond;
    return spins > timeout_ms * 1000;
}

static void ohci_delay_ms(uint64_t ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; !ohci_timed_out(start, spins, ms); spins++)
        asm volatile("pause");
}

// wait for (reg & mask) to become set or clear
static bool ohci_wait(uint64_t base, uint32_t reg, uint32_t mask,
                      bool want_set, uint64_t timeout_ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        if (!!(ohci_read(base, reg) & mask) == want_set)
            return true;
        if (ohci_timed_out(start, spins, timeout_ms))
            return false;
        asm volatile("pause");
    }
}

// drop a BAR into the HHDM, same trick ahci uses
static uint64_t usb_map_bar0(const pci_device_t *pci_dev, uint64_t size) {
    uint64_t phys = pci_dev->bar[0] & ~0xFull;
    if ((pci_dev->bar[0] & 0x6) == 0x4)     // 64bit BAR
        phys |= (uint64_t)pci_dev->bar[1] << 32;
    if (phys == 0)
        return 0;

    if (paging_map_mmio(phys, size) < 0)
        return 0;
    return MMIO(phys);
}

static void usb_pci_enable(const pci_device_t *pci_dev) {
    uint16_t cmd = pci_read16(pci_dev->bus, pci_dev->device, pci_dev->function,
                              PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
    pci_write16(pci_dev->bus, pci_dev->device, pci_dev->function,
                PCI_COMMAND, cmd);
}

// a usb2 controller sits in front of the ohci ones and while its CONFIGFLAG is
// set it owns every port, the keyboard would never show up on our side.
// we have no ehci driver so just take it off the firmware and switch it off
static void ehci_release_ports(void) {
    pci_device_t pci_dev;
    for (int n = 0; pci_find_device_nth(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                        PCI_PROGIF_EHCI, n, &pci_dev); n++) {
        usb_pci_enable(&pci_dev);

        uint64_t base = usb_map_bar0(&pci_dev, PAGE_SIZE_4K);
        if (!base) {
            serial_printf(LOG_WARN "ehci: %02x:%02x.%x has no usable BAR\n",
                          pci_dev.bus, pci_dev.device, pci_dev.function);
            continue;
        }

        // the legacy support capability lives in pci config space, ask the BIOS for ownership
        uint8_t eecp = (uint8_t)(ohci_read(base, EHCI_CAP_HCCPARAMS) >> 8);
        for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
            uint32_t cap = pci_read32(pci_dev.bus, pci_dev.device, pci_dev.function, eecp);
            if ((cap & 0xFF) == EHCI_LEGSUP_CAPID) {
                pci_write32(pci_dev.bus, pci_dev.device, pci_dev.function,
                            eecp, cap | EHCI_LEGSUP_OS);

                uint64_t start = hpet_get_femtoseconds();
                for (uint64_t spins = 0; ; spins++) {
                    cap = pci_read32(pci_dev.bus, pci_dev.device, pci_dev.function, eecp);
                    if (!(cap & EHCI_LEGSUP_BIOS))
                        break;
                    if (ohci_timed_out(start, spins, OHCI_HANDOFF_TIMEOUT_MS)) {
                        serial_printf(LOG_WARN "ehci: BIOS wont hand over, taking it anyway\n");
                        break;
                    }
                    asm volatile("pause");
                }
                // no more SMIs
                pci_write32(pci_dev.bus, pci_dev.device, pci_dev.function, eecp + 4, 0);
            }
            eecp = (uint8_t)(cap >> 8);
        }

        uint64_t op = base + (ohci_read(base, EHCI_CAP_LENGTH) & 0xFF);

        // it has to be halted before it will accept a reset
        ohci_write(op, EHCI_OP_USBINTR, 0);
        ohci_write(op, EHCI_OP_USBCMD, ohci_read(op, EHCI_OP_USBCMD) & ~EHCI_CMD_RUN);
        ohci_wait(op, EHCI_OP_USBSTS, EHCI_STS_HALTED, true, 50);

        ohci_write(op, EHCI_OP_USBCMD, EHCI_CMD_RESET);
        if (!ohci_wait(op, EHCI_OP_USBCMD, EHCI_CMD_RESET, false, 250))
            serial_printf(LOG_WARN "ehci: reset timeout\n");

        // route everything to the companion controllers
        ohci_write(op, EHCI_OP_CONFIGFLAG, 0);

        serial_printf(LOG_INFO "ehci: %02x:%02x.%x ports released to companions\n",
                      pci_dev.bus, pci_dev.device, pci_dev.function);
    }
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
static void ohci_collect_done(ohci_hc_t *hc) {
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

static int ohci_control_wait(ohci_hc_t *hc, int last_td) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        ohci_collect_done(hc);

        if (hc->ctrl_cc)
            return -1;
        if (hc->ctrl_retired & (1u << last_td))
            return 0;
        if (ohci_timed_out(start, spins, OHCI_CTRL_TIMEOUT_MS))
            return -2;
        asm volatile("pause");
    }
}

// setup -> (data) -> status, all on endpoint 0.
// not safe against itself, only ever called while probing with interrupts off
int ohci_control(usb_device_t *dev, const usb_setup_t *setup, void *data) {
    if (!dev || !dev->hc || !setup) return -1;

    ohci_hc_t  *hc  = dev->hc;
    ohci_dma_t *dma = hc->dma;
    ohci_ed_t  *ed  = &dma->eds[OHCI_ED_CONTROL];
    ohci_td_t  *td  = dma->tds;

    uint16_t len = setup->length;
    bool     in  = setup->request_type & USB_REQ_DIR_IN;

    if (len > sizeof(dma->ctrl_buf) || (len && !data)) return -1;

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
             | (dev->low_speed ? OHCI_ED_LOWSPEED : 0)
             | OHCI_ED_MPS(dev->max_packet0);

    ohci_write(hc->base, OHCI_REG_CMDSTATUS, OHCI_CMD_CLF);

    int r = ohci_control_wait(hc, OHCI_TD_STATUS);

    ed->info |= OHCI_ED_SKIP;
    if (r < 0) {
        // the controller might still be chewing on the ed, give it a frame
        // to notice the skip then sweep up whatever it retired
        ohci_delay_ms(2);
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

int ohci_interrupt_in(usb_device_t *dev, uint8_t endpoint, uint16_t max_packet,
                      usb_interrupt_cb_t cb, void *ctx) {
    if (!dev || !dev->hc || !cb) return -1;
    ohci_hc_t *hc = dev->hc;

    for (int i = 0; i < OHCI_MAX_PIPES; i++) {
        ohci_pipe_t *pipe = &hc->pipes[i];
        if (pipe->used) continue;

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
                       | (dev->low_speed ? OHCI_ED_LOWSPEED : 0)
                       | OHCI_ED_MPS(max_packet);

        ohci_pipe_arm(hc, pipe);
        return 0;
    }

    serial_printf(LOG_WARN "ohci: out of interrupt pipes\n");
    return -1;
}

static int usb_get_descriptor(usb_device_t *dev, uint8_t type, void *buf, uint16_t len) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_DEVICE,
        .request      = USB_REQ_GET_DESCRIPTOR,
        .value        = (uint16_t)(type << 8),
        .index        = 0,
        .length       = len,
    };
    return ohci_control(dev, &setup, buf);
}

// requests that have no data stage
static int usb_request(usb_device_t *dev, uint8_t request, uint16_t value) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_DEVICE,
        .request      = request,
        .value        = value,
    };
    return ohci_control(dev, &setup, NULL);
}

static bool ohci_port_reset(ohci_hc_t *hc, uint8_t port, bool *low_speed) {
    uint32_t reg = OHCI_REG_RH_PORT + (uint32_t)port * 4;

    if (!(ohci_read(hc->base, reg) & OHCI_PORT_CCS))
        return false;

    ohci_write(hc->base, reg, OHCI_PORT_PRS);
    if (!ohci_wait(hc->base, reg, OHCI_PORT_PRSC, true, OHCI_PORT_RESET_MS)) {
        serial_printf(LOG_WARN "ohci: port %u reset timeout\n", port);
        return false;
    }
    ohci_write(hc->base, reg, OHCI_PORT_CHANGE_MASK);

    // devices get 10ms to recover from a reset before you can talk to them
    ohci_delay_ms(10);

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

static void ohci_port_enumerate(ohci_hc_t *hc, uint8_t port) {
    bool low_speed = false;
    if (!ohci_port_reset(hc, port, &low_speed))
        return;

    if (hc->device_count >= OHCI_MAX_DEVICES || hc->next_address > 127) {
        serial_printf(LOG_WARN "ohci: port %u ignored, device table is full\n", port);
        return;
    }

    // fresh out of reset it answers on address 0
    usb_device_t *dev = &hc->devices[hc->device_count];
    memset(dev, 0, sizeof(*dev));
    dev->hc          = hc;
    dev->port        = port;
    dev->low_speed   = low_speed;
    dev->max_packet0 = 8;

    // only the first 8 bytes are safe to ask for until we know how big ep0 packets are
    usb_device_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, &desc, 8) < 8) {
        serial_printf(LOG_WARN "ohci: port %u device isnt answering\n", port);
        return;
    }
    if (desc.max_packet0 >= 8)
        dev->max_packet0 = desc.max_packet0;

    uint8_t address = hc->next_address++;
    if (usb_request(dev, USB_REQ_SET_ADDRESS, address) < 0) {
        serial_printf(LOG_WARN "ohci: port %u SET_ADDRESS failed\n", port);
        return;
    }
    ohci_delay_ms(2);   // and 2ms to start listening on the new one
    dev->address = address;

    if (usb_get_descriptor(dev, USB_DESC_DEVICE, &desc, sizeof(desc)) < (int)sizeof(desc)) {
        serial_printf(LOG_WARN "ohci: port %u cant read device descriptor\n", port);
        return;
    }
    dev->vendor_id  = desc.vendor_id;
    dev->product_id = desc.product_id;

    serial_printf(LOG_OK "ohci: port %u - %s speed device %04x:%04x at address %u\n",
                  port, low_speed ? "low" : "full",
                  dev->vendor_id, dev->product_id, dev->address);

    // the header tells us how long the whole config blob is
    uint8_t config[USB_MAX_CONFIG_LEN];
    usb_config_desc_t *cfg = (usb_config_desc_t *)config;
    if (usb_get_descriptor(dev, USB_DESC_CONFIG, config, sizeof(*cfg)) < (int)sizeof(*cfg)) {
        serial_printf(LOG_WARN "ohci: port %u cant read config descriptor\n", port);
        return;
    }

    uint16_t total = cfg->total_length;
    if (total > sizeof(config))
        total = sizeof(config);

    int got = usb_get_descriptor(dev, USB_DESC_CONFIG, config, total);
    if (got < (int)sizeof(*cfg)) {
        serial_printf(LOG_WARN "ohci: port %u cant read config descriptor\n", port);
        return;
    }

    if (usb_request(dev, USB_REQ_SET_CONFIG, cfg->config_value) < 0) {
        serial_printf(LOG_WARN "ohci: port %u SET_CONFIGURATION failed\n", port);
        return;
    }
    hc->device_count++;

    // a device can be both at once, the macbook keyboard and trackpad are one device
    bool claimed = usb_keyboard_probe(dev, config, (size_t)got);
    if (usb_mouse_probe(dev, config, (size_t)got))
        claimed = true;

    if (!claimed)
        serial_printf(LOG_INFO "ohci: port %u has nothing we drive\n", port);
}

// if SMM/BIOS is driving the controller (legacy keyboard emulation) ask for it nicely
static void ohci_take_ownership(uint64_t base) {
    uint32_t control = ohci_read(base, OHCI_REG_CONTROL);
    if (!(control & OHCI_CTRL_IR))
        return;

    ohci_write(base, OHCI_REG_INT_ENABLE, OHCI_INT_OC);
    ohci_write(base, OHCI_REG_CMDSTATUS, OHCI_CMD_OCR);

    if (!ohci_wait(base, OHCI_REG_CONTROL, OHCI_CTRL_IR, false, OHCI_HANDOFF_TIMEOUT_MS)) {
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
    hc->next_address = 1;
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
    if (!ohci_wait(base, OHCI_REG_CMDSTATUS, OHCI_CMD_HCR, false, OHCI_RESET_TIMEOUT_MS)) {
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
    ohci_delay_ms((rha >> OHCI_RHA_POTPGT_SHIFT) * 2 + 100);

    hc->present = true;
    serial_printf(LOG_INFO "ohci: rev %x.%x, %u ports\n",
                  (ohci_read(base, OHCI_REG_REVISION) >> 4) & 0xF,
                  ohci_read(base, OHCI_REG_REVISION) & 0xF, hc->port_count);

    for (uint8_t port = 0; port < hc->port_count; port++)
        ohci_port_enumerate(hc, port);

    return true;
}

void ohci_poll(void) {
    if (!ohci_ready) return;

    for (int i = 0; i < ohci_count; i++) {
        if (ohci_controllers[i].present)
            ohci_collect_done(&ohci_controllers[i]);
    }
}

void ohci_init(void) {
    memset(ohci_controllers, 0, sizeof(ohci_controllers));

    // has to happen first, the ports changing hands looks like a fresh attach on our side
    ehci_release_ports();

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
