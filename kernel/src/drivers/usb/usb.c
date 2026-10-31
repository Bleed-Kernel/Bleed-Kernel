#include <drivers/usb/usb.h>
#include <drivers/usb/ehci.h>
#include <drivers/usb/ohci.h>
#include <drivers/usb/xhci/xhci.h>
#include <drivers/usb/usb_keyboard.h>
#include <drivers/usb/usb_mouse.h>
#include <drivers/usb/usb_touch.h>
#include <drivers/usb/usb_storage.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <mm/spinlock.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static bool usb_ready = false;

const char *usb_speed_name(usb_speed_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:   return "low";
        case USB_SPEED_FULL:  return "full";
        case USB_SPEED_HIGH:  return "high";
        case USB_SPEED_SUPER: return "super";
    }
    return "unknown";
}

int usb_control(usb_device_t *dev, const usb_setup_t *setup, void *data) {
    if (!dev || !dev->ops || !dev->ops->control || !setup) return USB_ERR;
    if (dev->gone) return USB_ERR_GONE;
    return dev->ops->control(dev, setup, data);
}

int usb_get_descriptor(usb_device_t *dev, uint8_t type, void *buf, uint16_t len) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_DEVICE,
        .request      = USB_REQ_GET_DESCRIPTOR,
        .value        = (uint16_t)(type << 8),
        .index        = 0,
        .length       = len,
    };
    return usb_control(dev, &setup, buf);
}

int usb_request(usb_device_t *dev, uint8_t request, uint16_t value) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_DEVICE,
        .request      = request,
        .value        = value,
    };
    return usb_control(dev, &setup, NULL);
}

int usb_interrupt_in(usb_device_t *dev, const usb_endpoint_desc_t *ep,
                     usb_interrupt_cb_t cb, void *ctx) {
    if (!dev || !dev->ops || !dev->ops->interrupt_in || !ep || !cb) return USB_ERR;
    return dev->ops->interrupt_in(dev, ep, cb, ctx);
}

int usb_bulk_open(usb_device_t *dev, const usb_endpoint_desc_t *ep,
                  const usb_ss_companion_desc_t *ss) {
    if (!dev || !dev->ops || !dev->ops->bulk_open || !ep) return USB_ERR;
    return dev->ops->bulk_open(dev, ep, ss);
}

int usb_bulk(usb_device_t *dev, uint8_t endpoint, void *buf, size_t len) {
    if (!dev || !dev->ops || !dev->ops->bulk) return USB_ERR;
    if (dev->gone) return USB_ERR_GONE;
    return dev->ops->bulk(dev, endpoint, buf, len);
}

int usb_clear_halt(usb_device_t *dev, uint8_t endpoint) {
    if (!dev || !dev->ops) return USB_ERR;
    if (dev->gone) return USB_ERR_GONE;

    // controller side first, xhci wont touch the ring again until its endpoint is reset
    if (dev->ops->reset_endpoint && dev->ops->reset_endpoint(dev, endpoint) < 0) {
        serial_printf(LOG_WARN "usb: %s couldnt reset endpoint 0x%02x\n", dev->ops->name, endpoint);
        return USB_ERR;
    }

    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_ENDPOINT,
        .request      = USB_REQ_CLEAR_FEATURE,
        .value        = USB_FEATURE_ENDPOINT_HALT,
        .index        = endpoint,
    };
    int r = usb_control(dev, &setup, NULL);
    if (r < 0)
        serial_printf(LOG_WARN "usb: CLEAR_FEATURE(HALT) on endpoint 0x%02x failed (%d)\n", endpoint, r);
    else
        serial_printf(LOG_INFO "usb: endpoint 0x%02x unhalted\n", endpoint);
    return r;
}

// one line per descriptor so a log from real hardware tells us what the device actually is
static void usb_dump_config(const uint8_t *config, size_t len) {
    for (size_t off = 0; off + 2 <= len; ) {
        uint8_t desc_len  = config[off];
        uint8_t desc_type = config[off + 1];
        if (desc_len < 2 || off + desc_len > len) break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t *i = (const usb_interface_desc_t *)&config[off];
            serial_printf(LOG_INFO "usb:   interface %u alt %u - class %02x:%02x:%02x, %u endpoints\n",
                          i->interface_number, i->alt_setting, i->class_code,
                          i->subclass, i->protocol, i->endpoint_count);
        } else if (desc_type == USB_DESC_ENDPOINT && desc_len >= sizeof(usb_endpoint_desc_t)) {
            static const char *types[] = { "control", "isoch", "bulk", "interrupt" };
            const usb_endpoint_desc_t *e = (const usb_endpoint_desc_t *)&config[off];
            serial_printf(LOG_INFO "usb:     endpoint 0x%02x %s %s, max packet %u, interval %u\n",
                          e->address, (e->address & USB_ENDPOINT_IN) ? "in" : "out",
                          types[e->attributes & USB_ENDPOINT_TYPE_MASK],
                          e->max_packet & USB_ENDPOINT_MPS_MASK, e->interval);
        } else if (desc_type == USB_DESC_SS_COMPANION && desc_len >= sizeof(usb_ss_companion_desc_t)) {
            const usb_ss_companion_desc_t *c = (const usb_ss_companion_desc_t *)&config[off];
            serial_printf(LOG_INFO "usb:     ss companion - burst %u, attributes 0x%02x\n",
                          c->max_burst, c->attributes);
        }

        off += desc_len;
    }
}

bool usb_device_configure(usb_device_t *dev) {
    usb_device_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (usb_get_descriptor(dev, USB_DESC_DEVICE, &desc, sizeof(desc)) < (int)sizeof(desc)) {
        serial_printf(LOG_WARN "usb: port %u cant read device descriptor\n", dev->port);
        return false;
    }
    dev->vendor_id  = desc.vendor_id;
    dev->product_id = desc.product_id;

    serial_printf(LOG_OK "usb: %s port %u - %s speed device %04x:%04x at address %u\n",
                  dev->ops->name, dev->port, usb_speed_name(dev->speed),
                  dev->vendor_id, dev->product_id, dev->address);
    serial_printf(LOG_INFO "usb:   usb %x.%02x, class %02x:%02x:%02x, ep0 %u bytes, %u configs\n",
                  desc.usb_version >> 8, desc.usb_version & 0xFF, desc.class_code,
                  desc.subclass, desc.protocol, dev->max_packet0, desc.config_count);

    // the header tells us how long the whole config blob is
    uint8_t config[USB_MAX_CONFIG_LEN];
    usb_config_desc_t *cfg = (usb_config_desc_t *)config;
    if (usb_get_descriptor(dev, USB_DESC_CONFIG, config, sizeof(*cfg)) < (int)sizeof(*cfg)) {
        serial_printf(LOG_WARN "usb: port %u cant read config descriptor\n", dev->port);
        return false;
    }

    uint16_t total = cfg->total_length;
    if (total > sizeof(config)) {
        serial_printf(LOG_WARN "usb: config descriptor is %u bytes, only reading %u\n",
                      total, (unsigned)sizeof(config));
        total = sizeof(config);
    }

    int got = usb_get_descriptor(dev, USB_DESC_CONFIG, config, total);
    if (got < (int)sizeof(*cfg)) {
        serial_printf(LOG_WARN "usb: port %u cant read config descriptor\n", dev->port);
        return false;
    }

    serial_printf(LOG_INFO "usb:   config %u, %u interfaces, %d bytes\n",
                  cfg->config_value, cfg->interface_count, got);
    usb_dump_config(config, (size_t)got);

    if (usb_request(dev, USB_REQ_SET_CONFIG, cfg->config_value) < 0) {
        serial_printf(LOG_WARN "usb: port %u SET_CONFIGURATION failed\n", dev->port);
        return false;
    }

    // a device can be several at once, the macbook keyboard and trackpad are one device???
    // touch goes first, a tablet can claim to be a boot mouse and would get read as one
    bool claimed = usb_touch_probe(dev, config, (size_t)got);
    if (usb_keyboard_probe(dev, config, (size_t)got))
        claimed = true;
    if (usb_mouse_probe(dev, config, (size_t)got))
        claimed = true;
    if (usb_storage_probe(dev, config, (size_t)got))
        claimed = true;

    if (!claimed)
        serial_printf(LOG_INFO "usb: port %u has nothing we drive\n", dev->port);
    return true;
}

void usb_device_disconnect(usb_device_t *dev) {
    serial_printf(LOG_INFO "usb: %s port %u - device %04x:%04x unplugged\n",
                  dev->ops->name, dev->port, dev->vendor_id, dev->product_id);

    // first so anything mid transfer bails out instead of sitting through a frustrating timeout
    dev->gone = true;

    usb_keyboard_disconnect(dev);
    usb_mouse_disconnect(dev);
    usb_touch_disconnect(dev);
    usb_storage_disconnect(dev);
}

void usb_init(void) {
    // xhci goes first, on intel it pulls the shared ports away from ehci
    xhci_init();

    // the ports changing hands looks like a fresh attach
    ehci_release_ports();
    ohci_init();

    usb_ready = true;
}

void usb_poll(void) {
    if (!usb_ready) return;

    ohci_poll();
    xhci_poll();
    usb_keyboard_tick();
}

void usb_hotplug_watch(void) {
    serial_printf(LOG_INFO "usb: hotplug service running, checking ports every %ums\n",
                  USB_HOTPLUG_POLL_MS);

    for (;;) {
        uint64_t deadline = sched_deadline_ms(USB_HOTPLUG_POLL_MS);
        while (!sched_deadline_passed(deadline)) {
            unsigned long flags = irq_push();
            sched_sleep_until(deadline);
            irq_restore(flags);
        }

        // the drivers and vfs locks dont turn interrupts off
        unsigned long flags = irq_push();
        xhci_hotplug();
        ohci_hotplug();
        usb_storage_automount();
        irq_restore(flags);
    }
}
