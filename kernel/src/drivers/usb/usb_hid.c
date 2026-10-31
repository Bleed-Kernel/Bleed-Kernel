#include <drivers/usb/usb_hid.h>
#include <drivers/usb/usb.h>

bool usb_hid_boot_probe(usb_device_t *dev, const uint8_t *config, size_t len,
                        uint8_t protocol, usb_hid_attach_t attach) {
    const usb_interface_desc_t *iface = NULL;
    bool claimed = false;

    // the config descriptor is every interface and endpoint descriptor back to back
    for (size_t off = 0; off + 2 <= len; ) {
        uint8_t desc_len  = config[off];
        uint8_t desc_type = config[off + 1];
        if (desc_len < 2 || off + desc_len > len) break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= sizeof(usb_interface_desc_t)) {
            iface = (const usb_interface_desc_t *)&config[off];
            if (iface->class_code != USB_CLASS_HID ||
                iface->subclass   != USB_HID_SUBCLASS_BOOT ||
                iface->protocol   != protocol)
                iface = NULL;
        } else if (desc_type == USB_DESC_ENDPOINT && iface &&
                   desc_len >= sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t *)&config[off];

            if ((ep->address & USB_ENDPOINT_IN) &&
                (ep->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT &&
                !usb_interface_claimed(dev, iface->interface_number)) {
                if (attach(dev, iface->interface_number, ep)) {
                    usb_interface_claim(dev, iface->interface_number);
                    claimed = true;
                }
                iface = NULL;   // one endpoint per interface is all we want
            }
        }

        off += desc_len;
    }

    return claimed;
}

static int usb_hid_set_protocol(usb_device_t *dev, uint8_t interface, uint16_t protocol) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_INTERFACE,
        .request      = USB_HID_SET_PROTOCOL,
        .value        = protocol,
        .index        = interface,
    };
    int r = usb_control(dev, &setup, NULL);

    // idle rate 0 = only talk when something changed, some devices
    // stall this and thats allowed so the result doesnt matter
    setup.request = USB_HID_SET_IDLE;
    setup.value   = 0;
    usb_control(dev, &setup, NULL);

    return r;
}

// devices are allowed to wake up in report protocol, make sure its the boot one
int usb_hid_set_boot_protocol(usb_device_t *dev, uint8_t interface) {
    return usb_hid_set_protocol(dev, interface, USB_HID_PROTOCOL_BOOT);
}

int usb_hid_set_report_protocol(usb_device_t *dev, uint8_t interface) {
    return usb_hid_set_protocol(dev, interface, USB_HID_PROTOCOL_REPORT);
}

int usb_hid_get_report_desc(usb_device_t *dev, uint8_t interface, void *buf, uint16_t len) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RCPT_INTERFACE,
        .request      = USB_REQ_GET_DESCRIPTOR,
        .value        = (uint16_t)(USB_DESC_HID_REPORT << 8),
        .index        = interface,
        .length       = len,
    };
    return usb_control(dev, &setup, buf);
}

bool usb_interface_claimed(const usb_device_t *dev, uint8_t interface) {
    return interface < 32 && (dev->claimed & (1u << interface));
}

void usb_interface_claim(usb_device_t *dev, uint8_t interface) {
    if (interface < 32)
        dev->claimed |= 1u << interface;
}
