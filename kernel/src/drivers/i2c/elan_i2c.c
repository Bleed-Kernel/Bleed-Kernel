#include <drivers/i2c/elan_i2c.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/serial/serial.h>
#include <input/mouse_dispatch.h>
#include <input/mouse_input.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static elan_dev_t trackpad;

static int elan_read_block(elan_dev_t *dev, uint16_t reg, void *buf, size_t len) {
    uint8_t out[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) };
    return dw_i2c_xfer(dev->bus, dev->addr, out, sizeof(out), buf, len);
}

static int elan_read_cmd(elan_dev_t *dev, uint16_t reg, uint16_t *val) {
    uint8_t in[2];
    if (elan_read_block(dev, reg, in, sizeof(in)) != (int)sizeof(in)) return -1;

    *val = in[0] | (in[1] << 8);
    return 0;
}

static int elan_write_cmd(elan_dev_t *dev, uint16_t reg, uint16_t cmd) {
    uint8_t out[4] = { (uint8_t)reg, (uint8_t)(reg >> 8), (uint8_t)cmd, (uint8_t)(cmd >> 8) };
    return dw_i2c_xfer(dev->bus, dev->addr, out, sizeof(out), NULL, 0);
}

// pad units to pixels, the remainder is kept so slow movement isnt rounded away
static int16_t elan_scale(elan_dev_t *dev, int32_t delta, int32_t *rem) {
    int32_t accum  = delta * ELAN_POINTER_SPAN + *rem;
    int32_t scaled = accum / dev->max_x;
    *rem = accum % dev->max_x;
    return (int16_t)scaled;
}

static void elan_send(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons) {
    mouse_event_t ev = {
        .dx      = dx,
        .dy      = dy,
        .wheel   = wheel,
        .buttons = buttons
    };
    mouse_input_dispatch(&ev);
}

// the pad only says where fingers are, everything a mouse does is worked out here.
// one finger moves, two scroll, a quick touch is a click
static bool elan_report(elan_dev_t *dev, const uint8_t *report) {
    uint8_t info  = report[ELAN_TOUCH_INFO_OFFSET];
    bool    hires = report[ELAN_REPORT_ID_OFFSET] == ELAN_REPORT_ID_HIRES;

    uint8_t  fingers = 0;
    uint8_t  slot    = 0;
    uint16_t x       = 0;
    uint16_t y       = 0;

    const uint8_t *finger = &report[ELAN_FINGER_OFFSET];
    for (uint8_t i = 0; i < ELAN_MAX_FINGERS; i++) {
        if (!((info >> (3 + i)) & 1)) continue;

        // the pointer follows whichever finger comes first
        if (fingers++ == 0) {
            slot = i;
            if (hires) {
                x = (finger[0] << 8) | finger[1];
                y = (finger[2] << 8) | finger[3];
            } else {
                x = ((finger[0] & 0xF0) << 4) | finger[1];
                y = ((finger[0] & 0x0F) << 8) | finger[2];
            }
        }
        finger += ELAN_FINGER_LENGTH;
    }

    // a finger arriving or leaving changes which one that is, dont turn it into a jump.
    // the pad counts y from the bottom edge
    int32_t dx = 0;
    int32_t dy = 0;
    if (fingers && fingers == dev->fingers && slot == dev->slot) {
        dx = (int32_t)x - dev->last_x;
        dy = (int32_t)dev->last_y - y;
    }

    int32_t travel = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (travel > dev->max_x / ELAN_JUMP_DIV) {
        dx = 0;
        dy = 0;
    }

    uint64_t now = hpet_get_femtoseconds() / femtosecondsPerMillisecond;
    if (fingers && !dev->fingers) {
        dev->tap_start   = now;
        dev->tap_fingers = 0;
        dev->tap_travel  = 0;
    }
    if (fingers) {
        if (fingers > dev->tap_fingers) dev->tap_fingers = fingers;
        dev->tap_travel += (uint32_t)travel;

        // pressing the pad down is a click of its own
        if (info & (ELAN_INFO_LEFT | ELAN_INFO_RIGHT | ELAN_INFO_MIDDLE))
            dev->tap_travel = UINT32_MAX / 2;
    }
    bool tapped = !fingers && dev->fingers &&
                  now - dev->tap_start <= ELAN_TAP_MS &&
                  dev->tap_travel <= (uint32_t)dev->max_x / ELAN_TAP_SLOP_DIV;

    // theres one switch under the whole pad, two fingers on it makes it the right button
    uint8_t buttons = (info & ELAN_INFO_RIGHT  ? MOUSE_BTN_RIGHT  : 0) |
                      (info & ELAN_INFO_MIDDLE ? MOUSE_BTN_MIDDLE : 0);
    if (info & ELAN_INFO_LEFT)
        buttons |= fingers >= 2 ? MOUSE_BTN_RIGHT : MOUSE_BTN_LEFT;

    int16_t move_x = 0;
    int16_t move_y = 0;
    int8_t  wheel  = 0;

    if (fingers == 1) {
        move_x = elan_scale(dev, dx, &dev->rem_x);
        move_y = elan_scale(dev, dy, &dev->rem_y);
    } else if (fingers == 2) {
        // fingers going up the pad is the wheel rolling away from you
        int32_t notch = dev->max_y / ELAN_SCROLL_NOTCHES;
        if (notch < 1) notch = 1;

        dev->scroll -= dy;
        wheel        = (int8_t)(dev->scroll / notch);
        dev->scroll -= wheel * notch;
    }
    if (fingers != 2)
        dev->scroll = 0;

    if (move_x || move_y || wheel || buttons != dev->buttons)
        elan_send(move_x, move_y, wheel, buttons);

    if (tapped) {
        uint8_t tap = dev->tap_fingers >= 2 ? MOUSE_BTN_RIGHT : MOUSE_BTN_LEFT;
        elan_send(0, 0, 0, buttons | tap);
        elan_send(0, 0, 0, buttons);
    }

    dev->fingers = fingers;
    dev->slot    = slot;
    dev->last_x  = x;
    dev->last_y  = y;
    dev->buttons = buttons;

    return fingers || buttons || tapped;
}

bool elan_i2c_poll(void) {
    elan_dev_t *dev = &trackpad;
    if (!dev->present) return false;

    uint8_t report[ELAN_REPORT_LENGTH];
    if (dw_i2c_xfer(dev->bus, dev->addr, NULL, 0, report, sizeof(report)) != (int)sizeof(report)) {
        if (++dev->errors >= ELAN_MAX_ERRORS) {
            serial_printf(LOG_WARN "elan: trackpad stopped answering, giving up on it\n");
            dev->present = false;
        }
        return false;
    }
    dev->errors = 0;

    // read with nothing new it can hand back anything, the last report again is fine
    // since positions dont add up the way deltas would
    uint8_t id = report[ELAN_REPORT_ID_OFFSET];
    if (id != ELAN_REPORT_ID && id != ELAN_REPORT_ID_HIRES)
        return false;

    return elan_report(dev, report);
}

bool elan_i2c_probe(dw_i2c_t *bus, int bus_index, uint8_t addr) {
    elan_dev_t *dev = &trackpad;

    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = addr;

    if (elan_write_cmd(dev, ELAN_REG_STAND, ELAN_STAND_RESET) < 0)
        return false;
    wait_ms(ELAN_RESET_MS);

    // the reset is acked with two zeros, and it wants both descriptors read before
    // it will do anything else. none of it tells us anything we use
    uint8_t scratch[ELAN_REPORT_DESC_LENGTH];
    dw_i2c_xfer(bus, addr, NULL, 0, scratch, 2);

    if (elan_read_block(dev, ELAN_REG_DESC, scratch, ELAN_DESC_LENGTH) < 0 ||
        elan_read_block(dev, ELAN_REG_REPORT_DESC, scratch, ELAN_REPORT_DESC_LENGTH) < 0) {
        serial_printf(LOG_WARN "elan: bus %d 0x%02x wont hand over its descriptors\n", bus_index, addr);
        return false;
    }

    if (elan_read_cmd(dev, ELAN_REG_MAX_X, &dev->max_x) < 0 ||
        elan_read_cmd(dev, ELAN_REG_MAX_Y, &dev->max_y) < 0)
        return false;

    // only the low 12 bits are the size
    dev->max_x &= 0x0FFF;
    dev->max_y &= 0x0FFF;
    if (!dev->max_x || !dev->max_y || dev->max_x == 0x0FFF) {
        serial_printf(LOG_INFO "elan: bus %d 0x%02x isnt an Elan trackpad (size %ux%u)\n",
                      bus_index, addr, dev->max_x, dev->max_y);
        return false;
    }

    if (elan_write_cmd(dev, ELAN_REG_SET, ELAN_SET_ABSOLUTE) < 0 ||
        elan_write_cmd(dev, ELAN_REG_STAND, ELAN_STAND_WAKE_UP) < 0) {
        serial_printf(LOG_WARN "elan: bus %d 0x%02x wouldnt go into absolute mode\n", bus_index, addr);
        return false;
    }

    dev->present = true;
    serial_printf(LOG_OK "elan: trackpad on bus %d at 0x%02x, %ux%u\n",
                  bus_index, addr, dev->max_x, dev->max_y);
    return true;
}
