#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/usb/usb.h>

#define USB_KBD_MAX             4
#define USB_KBD_REPORT_SIZE     8       // boot protocol: modifiers, reserved, 6 keys

#define USB_KBD_REPEAT_DELAY_MS 500
#define USB_KBD_REPEAT_RATE_MS  33

// modifier byte bits in the boot report
#define USB_KBD_MOD_LCTRL       (1 << 0)
#define USB_KBD_MOD_LSHIFT      (1 << 1)
#define USB_KBD_MOD_LALT        (1 << 2)
#define USB_KBD_MOD_LGUI        (1 << 3)
#define USB_KBD_MOD_RCTRL       (1 << 4)
#define USB_KBD_MOD_RSHIFT      (1 << 5)
#define USB_KBD_MOD_RALT        (1 << 6)
#define USB_KBD_MOD_RGUI        (1 << 7)

#define USB_KBD_USAGE_ROLLOVER  0x01    // too many keys held, report is junk
#define USB_KBD_USAGE_CAPSLOCK  0x39

/// @brief Look through a config descriptor for a boot keyboard and start it
/// @param dev configured device
/// @param config full config descriptor
/// @param len length of config
/// @return true if a keyboard interface was claimed
bool usb_keyboard_probe(usb_device_t *dev, const uint8_t *config, size_t len);

/// @brief key repeat, usb keyboards dont do typematic themselves like PS2 ones
void usb_keyboard_tick(void);
