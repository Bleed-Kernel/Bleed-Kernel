#include <drivers/usb/usb_mouse.h>
#include <drivers/usb/usb_hid.h>
#include <drivers/usb/ohci.h>
#include <drivers/usb/usb.h>
#include <drivers/serial/serial.h>
#include <input/mouse_dispatch.h>
#include <input/mouse_input.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

typedef struct usb_mouse {
    bool          used;
    usb_device_t *dev;
    uint8_t       interface;
} usb_mouse_t;

static usb_mouse_t mice[USB_MOUSE_MAX];

// unlike the keyboard a mouse report is already an event, deltas since the last one
static void usb_mouse_report(void *ctx, const uint8_t *report, size_t len) {
    (void)ctx;
    if (len < USB_MOUSE_REPORT_SIZE) return;

    // usb counts +y as towards you which is already down the screen, PS/2 is
    // the one thats upside down so no flip here
    mouse_event_t ev = {
        .dx      = (int8_t)report[1],
        .dy      = (int8_t)report[2],
        // not part of the boot report but most mice tack the wheel on the end anyway
        .wheel   = len > USB_MOUSE_REPORT_SIZE ? (int8_t)report[3] : 0,
        .buttons =
            (report[0] & USB_MOUSE_BTN_LEFT   ? MOUSE_BTN_LEFT   : 0) |
            (report[0] & USB_MOUSE_BTN_RIGHT  ? MOUSE_BTN_RIGHT  : 0) |
            (report[0] & USB_MOUSE_BTN_MIDDLE ? MOUSE_BTN_MIDDLE : 0)
    };

    mouse_input_dispatch(&ev);
}

static bool usb_mouse_attach(usb_device_t *dev, uint8_t interface,
                             const usb_endpoint_desc_t *ep) {
    usb_mouse_t *mouse = NULL;
    for (int i = 0; i < USB_MOUSE_MAX; i++) {
        if (!mice[i].used) {
            mouse = &mice[i];
            break;
        }
    }
    if (!mouse) {
        serial_printf(LOG_WARN "usbmouse: too many mice\n");
        return false;
    }

    memset(mouse, 0, sizeof(*mouse));
    mouse->dev       = dev;
    mouse->interface = interface;

    if (usb_hid_set_boot_protocol(dev, interface) < 0)
        serial_printf(LOG_WARN "usbmouse: SET_PROTOCOL failed, hoping its in boot protocol already\n");

    if (ohci_interrupt_in(dev, ep->address, ep->max_packet & 0x7FF,
                          usb_mouse_report, mouse) < 0)
        return false;

    mouse->used = true;
    serial_printf(LOG_OK "usbmouse: mouse %04x:%04x on interface %u, endpoint 0x%02x\n",
                  dev->vendor_id, dev->product_id, interface, ep->address);
    return true;
}

bool usb_mouse_probe(usb_device_t *dev, const uint8_t *config, size_t len) {
    return usb_hid_boot_probe(dev, config, len, USB_HID_PROTO_MOUSE, usb_mouse_attach);
}
