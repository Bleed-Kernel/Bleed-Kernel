#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/usb/usb.h>

// a driver hands one of these to usb_hid_boot_probe, it gets called for each interface that matches
typedef bool (*usb_hid_attach_t)(usb_device_t *dev, uint8_t interface,
                                 const usb_endpoint_desc_t *ep);

/// @brief Look through a config descriptor for boot protocol HID interfaces
/// @param dev configured device
/// @param config full config descriptor
/// @param len length of config
/// @param protocol USB_HID_PROTO_KEYBOARD or USB_HID_PROTO_MOUSE
/// @param attach called with the interface and its interrupt IN endpoint
/// @return true if attach claimed at least one interface
bool usb_hid_boot_probe(usb_device_t *dev, const uint8_t *config, size_t len,
                        uint8_t protocol, usb_hid_attach_t attach);

// Put an interface into boot protocol and tell it to only report changes
// returns negative if the device refused SET_PROTOCOL
int usb_hid_set_boot_protocol(usb_device_t *dev, uint8_t interface);
