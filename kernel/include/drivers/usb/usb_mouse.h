#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/usb/usb.h>

#define USB_MOUSE_MAX           4
#define USB_MOUSE_REPORT_SIZE   3       // boot protocol: buttons, dx, dy

// button byte bits in the boot report
#define USB_MOUSE_BTN_LEFT      (1 << 0)
#define USB_MOUSE_BTN_RIGHT     (1 << 1)
#define USB_MOUSE_BTN_MIDDLE    (1 << 2)

/// @brief Look through a config descriptor for a boot mouse and start it
/// this is also what a macbook trackpad looks like until something switches it to multitouch
/// @param dev configured device
/// @param config full config descriptor
/// @param len length of config
/// @return true if a mouse interface was claimed
bool usb_mouse_probe(usb_device_t *dev, const uint8_t *config, size_t len);
