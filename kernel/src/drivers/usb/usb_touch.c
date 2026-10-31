#include <drivers/usb/usb_touch.h>
#include <drivers/usb/usb_hid.h>
#include <drivers/usb/usb.h>
#include <drivers/hid/hid.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

typedef struct usb_touch {
    bool          used;
    usb_device_t *dev;
    uint8_t       interface;
    hid_input_t   hid;
} usb_touch_t;

static usb_touch_t touches[USB_TOUCH_MAX];

static void usb_touch_report(void *ctx, const uint8_t *report, size_t len) {
    usb_touch_t *touch = ctx;
    if (!touch->used) return;
    hid_report(&touch->hid, report, len);
}

static bool usb_touch_attach(usb_device_t *dev, const usb_interface_desc_t *iface,
                             const usb_endpoint_desc_t *ep, uint16_t rd_len) {
    usb_touch_t *touch = NULL;
    for (int i = 0; i < USB_TOUCH_MAX && !touch; i++) {
        if (!touches[i].used)
            touch = &touches[i];
    }
    if (!touch) return false;

    if (rd_len > HID_MAX_REPORT_DESC) {
        serial_printf(LOG_WARN "usbtouch: interface %u report descriptor is %u bytes, too big\n",
                      iface->interface_number, rd_len);
        return false;
    }

    uint8_t *rd = kmalloc(rd_len);
    if (!rd) return false;

    int got = usb_hid_get_report_desc(dev, iface->interface_number, rd, rd_len);
    if (got <= 0) {
        serial_printf(LOG_WARN "usbtouch: interface %u wont give its report descriptor (%d)\n",
                      iface->interface_number, got);
        kfree(rd);
        return false;
    }

    memset(touch, 0, sizeof(*touch));
    hid_parse(&touch->hid, rd, (size_t)got);
    kfree(rd);

    // plain mice and keyboards are the boot drivers problem
    if (!touch->hid.has_touch && !(touch->hid.has_mouse && touch->hid.mouse_absolute))
        return false;

    hid_describe(&touch->hid, "usbtouch");
    touch->dev       = dev;
    touch->interface = iface->interface_number;

    // a boot capable one might be sat in boot protocol, which has no room for absolute anything
    if (usb_hid_set_report_protocol(dev, iface->interface_number) < 0 &&
        iface->subclass == USB_HID_SUBCLASS_BOOT)
        serial_printf(LOG_WARN "usbtouch: SET_PROTOCOL failed, hoping its in report protocol already\n");

    if (usb_interrupt_in(dev, ep, usb_touch_report, touch) < 0)
        return false;

    touch->used = true;
    serial_printf(LOG_OK "usbtouch: %s %04x:%04x on interface %u, endpoint 0x%02x\n",
                  touch->hid.has_touch ? "touchscreen" : "tablet", dev->vendor_id, dev->product_id,
                  iface->interface_number, ep->address);
    return true;
}

// the HID descriptor right after each interface says how long its report descriptor is
bool usb_touch_probe(usb_device_t *dev, const uint8_t *config, size_t len) {
    const usb_interface_desc_t *iface   = NULL;
    uint16_t                    rd_len  = 0;
    bool                        claimed = false;

    for (size_t off = 0; off + 2 <= len; ) {
        uint8_t desc_len  = config[off];
        uint8_t desc_type = config[off + 1];
        if (desc_len < 2 || off + desc_len > len) break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= sizeof(usb_interface_desc_t)) {
            iface  = (const usb_interface_desc_t *)&config[off];
            rd_len = 0;
            if (iface->class_code != USB_CLASS_HID)
                iface = NULL;
        } else if (desc_type == USB_DESC_HID && iface && desc_len >= USB_HID_DESC_MIN_LEN) {
            rd_len = config[off + USB_HID_DESC_RD_LEN] | (uint16_t)(config[off + USB_HID_DESC_RD_LEN + 1] << 8);
        } else if (desc_type == USB_DESC_ENDPOINT && iface &&
                   desc_len >= sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t *)&config[off];

            if ((ep->address & USB_ENDPOINT_IN) &&
                (ep->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT) {
                if (rd_len && !usb_interface_claimed(dev, iface->interface_number) &&
                    usb_touch_attach(dev, iface, ep, rd_len)) {
                    usb_interface_claim(dev, iface->interface_number);
                    claimed = true;
                }
                iface = NULL;
            }
        }

        off += desc_len;
    }

    return claimed;
}

void usb_touch_disconnect(usb_device_t *dev) {
    for (int i = 0; i < USB_TOUCH_MAX; i++) {
        usb_touch_t *touch = &touches[i];
        if (!touch->used || touch->dev != dev) continue;

        unsigned long flags = irq_push();
        hid_release(&touch->hid);
        touch->used = false;
        touch->dev  = NULL;
        irq_restore(flags);

        serial_printf(LOG_INFO "usbtouch: %04x:%04x gone\n", dev->vendor_id, dev->product_id);
    }
}
