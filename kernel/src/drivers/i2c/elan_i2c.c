#include <drivers/i2c/elan_i2c.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/serial/serial.h>
#include <input/mouse_input.h>
#include <ACPI/acpi_hpet.h>
#include <input/trackpad_gesture.h>
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

// the pad only says where fingers are, trackpad_gesture works out the rest
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

    // the pad counts y from the bottom edge
    y = y < dev->max_y ? dev->max_y - y : 0;

    uint8_t switches = (info & ELAN_INFO_LEFT   ? MOUSE_BTN_LEFT   : 0) |
                       (info & ELAN_INFO_RIGHT  ? MOUSE_BTN_RIGHT  : 0) |
                       (info & ELAN_INFO_MIDDLE ? MOUSE_BTN_MIDDLE : 0);
    return trackpad_gesture_frame(&dev->pad, fingers, slot, x, y, switches);
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

    dev->pad.max_x = dev->max_x;
    dev->pad.max_y = dev->max_y;
    dev->present   = true;
    serial_printf(LOG_OK "elan: trackpad on bus %d at 0x%02x, %ux%u\n",
                  bus_index, addr, dev->max_x, dev->max_y);
    return true;
}
