#include <drivers/usb/usb_hid.h>
#include <drivers/usb/ohci.h>
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
                (ep->attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_INTERRUPT) {
                if (attach(dev, iface->interface_number, ep))
                    claimed = true;
                iface = NULL;   // one endpoint per interface is all we want
            }
        }

        off += desc_len;
    }

    return claimed;
}

int usb_hid_set_boot_protocol(usb_device_t *dev, uint8_t interface) {
    // devices are allowed to wake up in report protocol, make sure its the boot one
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_INTERFACE,
        .request      = USB_HID_SET_PROTOCOL,
        .value        = USB_HID_PROTOCOL_BOOT,
        .index        = interface,
    };
    int r = ohci_control(dev, &setup, NULL);

    // idle rate 0 = only talk when something changed, some devices
    // stall this and thats allowed so the result doesnt matter
    setup.request = USB_HID_SET_IDLE;
    setup.value   = 0;
    ohci_control(dev, &setup, NULL);

    return r;
}
