#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/usb/usb.h>

#define USB_TOUCH_MAX           4
#define USB_HID_DESC_MIN_LEN    9
#define USB_HID_DESC_RD_LEN     7

/// @brief Look through a config descriptor for touchscreens and tablets and start them
/// runs before the boot mouse driver so an absolute pointer isnt read as a relative one
/// @param dev configured device
/// @param config full config descriptor
/// @param len length of config
/// @return true if an interface was claimed
bool usb_touch_probe(usb_device_t *dev, const uint8_t *config, size_t len);

void usb_touch_disconnect(usb_device_t *dev);
