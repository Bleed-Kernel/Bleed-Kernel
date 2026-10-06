#include <drivers/usb/usb_keyboard.h>
#include <drivers/usb/usb_hid.h>
#include <drivers/usb/ohci.h>
#include <drivers/usb/usb.h>
#include <drivers/serial/serial.h>
#include <input/keyboard_dispatch.h>
#include <input/keyboard_input.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

typedef struct usb_keyboard {
    bool          used;
    usb_device_t *dev;
    uint8_t       interface;
    uint8_t       last[USB_KBD_REPORT_SIZE];    // previous report, we diff against it
} usb_keyboard_t;

static usb_keyboard_t keyboards[USB_KBD_MAX];

static bool    caps   = false;
static uint8_t keymod = 0;

// typematic
static uint8_t  repeat_scancode = 0;
static uint64_t repeat_next_ms  = 0;

// HID usage -> PS/2 set 1 scancode, the rest of the kernel only knows set 1 so we pretend to be a PS/2 keyboard.
// the PS/2 driver drops the E0 prefix so the arrows and friends share codes
// with the keypad, same deal here
static const uint8_t usage_to_set1[] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,    // a b c d
    [0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,    // e f g h
    [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,    // i j k l
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,    // m n o p
    [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,    // q r s t
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,    // u v w x
    [0x1C] = 0x15, [0x1D] = 0x2C,                                  // y z

    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,    // 1 2 3 4
    [0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,    // 5 6 7 8
    [0x26] = 0x0A, [0x27] = 0x0B,                                  // 9 0

    [0x28] = 0x1C,  // enter
    [0x29] = 0x01,  // escape
    [0x2A] = 0x0E,  // backspace
    [0x2B] = 0x0F,  // tab
    [0x2C] = 0x39,  // space
    [0x2D] = 0x0C,  // -
    [0x2E] = 0x0D,  // =
    [0x2F] = 0x1A,  // [
    [0x30] = 0x1B,  // ]
    [0x31] = 0x2B,  // backslash
    [0x32] = 0x2B,  // #
    [0x33] = 0x27,  // ;
    [0x34] = 0x28,  // '
    [0x35] = 0x29,  // `
    [0x36] = 0x33,  // ,
    [0x37] = 0x34,  // .
    [0x38] = 0x35,  // /
    [0x39] = 0x3A,  // caps lock

    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,    // F1 - F4
    [0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,    // F5 - F8
    [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,    // F9 - F12

    [0x46] = 0x37,  // print screen
    [0x47] = 0x46,  // scroll lock
    [0x49] = 0x52,  // insert
    [0x4A] = 0x47,  // home
    [0x4B] = 0x49,  // page up
    [0x4C] = 0x53,  // delete
    [0x4D] = 0x4F,  // end
    [0x4E] = 0x51,  // page down
    [0x4F] = 0x4D,  // right
    [0x50] = 0x4B,  // left
    [0x51] = 0x50,  // down
    [0x52] = 0x48,  // up

    [0x53] = 0x45,  // num lock
    [0x54] = 0x35,  // keypad /
    [0x55] = 0x37,  // keypad *
    [0x56] = 0x4A,  // keypad -
    [0x57] = 0x4E,  // keypad +
    [0x58] = 0x1C,  // keypad enter
    [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,                   // keypad 1 2 3
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D,                   // keypad 4 5 6
    [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,                   // keypad 7 8 9
    [0x62] = 0x52,  // keypad 0
    [0x63] = 0x53,  // keypad .
    [0x64] = 0x56,  // shift (again)
};

// the modifiers arent in the key array, they get a bit each in byte 0
static const struct {
    uint8_t bit;
    uint8_t scancode;
} modifier_keys[] = {
    { USB_KBD_MOD_LCTRL,  0x1D },
    { USB_KBD_MOD_LSHIFT, 0x2A },
    { USB_KBD_MOD_LALT,   0x38 },
    { USB_KBD_MOD_LGUI,   0x5B },
    { USB_KBD_MOD_RCTRL,  0x1D },
    { USB_KBD_MOD_RSHIFT, 0x36 },
    { USB_KBD_MOD_RALT,   0x38 },
    { USB_KBD_MOD_RGUI,   0x5C },
};

#define MODIFIER_KEY_COUNT (sizeof(modifier_keys) / sizeof(modifier_keys[0]))

static uint8_t usb_keyboard_scancode(uint8_t usage) {
    if (usage >= sizeof(usage_to_set1)) return 0;
    return usage_to_set1[usage];
}

static void usb_keyboard_update_keymod(uint8_t mods) {
    keymod =
        ((mods & (USB_KBD_MOD_LSHIFT | USB_KBD_MOD_RSHIFT)) ? KEYMOD_SHIFT : 0) |
        ((mods & (USB_KBD_MOD_LCTRL  | USB_KBD_MOD_RCTRL))  ? KEYMOD_CTRL  : 0) |
        ((mods & (USB_KBD_MOD_LALT   | USB_KBD_MOD_RALT))   ? KEYMOD_ALT   : 0) |
        (caps ? KEYMOD_CAPS : 0);
}

static void usb_keyboard_emit(uint8_t scancode, bool down) {
    if (!scancode) return;

    keyboard_event_t ev = {
        .keycode = scancode,
        .action  = down ? KEY_DOWN : KEY_RELEASE,
        .keymod  = keymod
    };

    keyboard_input_dispatch(&ev);
}

static bool usb_keyboard_holds(const uint8_t *report, uint8_t usage) {
    for (int i = 2; i < USB_KBD_REPORT_SIZE; i++) {
        if (report[i] == usage) return true;
    }
    return false;
}

// a report is the full set of keys held right now, not an event, so the
// presses and releases fall out of comparing it with the one before
static void usb_keyboard_report(void *ctx, const uint8_t *report, size_t len) {
    usb_keyboard_t *kbd = ctx;
    if (len < USB_KBD_REPORT_SIZE) return;

    // too many keys down, every slot reads rollover and tells us nothing
    if (report[2] == USB_KBD_USAGE_ROLLOVER) return;

    uint8_t changed = report[0] ^ kbd->last[0];
    usb_keyboard_update_keymod(report[0]);

    for (size_t i = 0; i < MODIFIER_KEY_COUNT; i++) {
        if (changed & modifier_keys[i].bit)
            usb_keyboard_emit(modifier_keys[i].scancode,
                              report[0] & modifier_keys[i].bit);
    }

    for (int i = 2; i < USB_KBD_REPORT_SIZE; i++) {
        uint8_t usage = kbd->last[i];
        if (!usage || usb_keyboard_holds(report, usage)) continue;

        uint8_t scancode = usb_keyboard_scancode(usage);
        if (scancode == repeat_scancode)
            repeat_scancode = 0;
        usb_keyboard_emit(scancode, false);
    }

    for (int i = 2; i < USB_KBD_REPORT_SIZE; i++) {
        uint8_t usage = report[i];
        if (!usage || usb_keyboard_holds(kbd->last, usage)) continue;

        uint8_t scancode = usb_keyboard_scancode(usage);
        if (usage == USB_KBD_USAGE_CAPSLOCK) {
            caps ^= 1;
            usb_keyboard_update_keymod(report[0]);
        } else if (scancode) {
            // newest key down is the one that repeats
            repeat_scancode = scancode;
            repeat_next_ms  = hpet_get_femtoseconds() / femtosecondsPerMillisecond
                            + USB_KBD_REPEAT_DELAY_MS;
        }
        usb_keyboard_emit(scancode, true);
    }

    memcpy(kbd->last, report, USB_KBD_REPORT_SIZE);
}

void usb_keyboard_tick(void) {
    if (!repeat_scancode) return;

    // no HPET means now is always 0 and nothing ever repeats, thats fine
    uint64_t now = hpet_get_femtoseconds() / femtosecondsPerMillisecond;
    if (now < repeat_next_ms) return;

    repeat_next_ms = now + USB_KBD_REPEAT_RATE_MS;
    usb_keyboard_emit(repeat_scancode, true);
}

static bool usb_keyboard_attach(usb_device_t *dev, uint8_t interface,
                                const usb_endpoint_desc_t *ep) {
    usb_keyboard_t *kbd = NULL;
    for (int i = 0; i < USB_KBD_MAX; i++) {
        if (!keyboards[i].used) {
            kbd = &keyboards[i];
            break;
        }
    }
    if (!kbd) {
        serial_printf(LOG_WARN "usbkbd: too many keyboards\n");
        return false;
    }

    memset(kbd, 0, sizeof(*kbd));
    kbd->dev       = dev;
    kbd->interface = interface;

    if (usb_hid_set_boot_protocol(dev, interface) < 0)
        serial_printf(LOG_WARN "usbkbd: SET_PROTOCOL failed, hoping its in boot protocol already\n");

    if (ohci_interrupt_in(dev, ep->address, ep->max_packet & 0x7FF,
                          usb_keyboard_report, kbd) < 0)
        return false;

    kbd->used = true;
    serial_printf(LOG_OK "usbkbd: keyboard %04x:%04x on interface %u, endpoint 0x%02x\n",
                  dev->vendor_id, dev->product_id, interface, ep->address);
    return true;
}

bool usb_keyboard_probe(usb_device_t *dev, const uint8_t *config, size_t len) {
    return usb_hid_boot_probe(dev, config, len, USB_HID_PROTO_KEYBOARD, usb_keyboard_attach);
}
