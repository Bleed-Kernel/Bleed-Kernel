#include <drivers/i2c/i2c_hid.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/serial/serial.h>
#include <input/mouse_dispatch.h>
#include <input/mouse_input.h>
#include <mm/kalloc.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static i2c_hid_dev_t trackpad;

// registers are 16 bits, low byte first
static int i2c_hid_read_reg(dw_i2c_t *bus, uint8_t addr, uint16_t reg,
                            void *buf, size_t len) {
    uint8_t out[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) };
    return dw_i2c_xfer(bus, addr, out, sizeof(out), buf, len);
}

// arg is the power state for SET_POWER, nothing else we send takes one
static int i2c_hid_command(i2c_hid_dev_t *dev, uint8_t opcode, uint8_t arg) {
    uint16_t reg    = dev->desc.command_reg;
    uint8_t  out[4] = { (uint8_t)reg, (uint8_t)(reg >> 8), arg, opcode };
    return dw_i2c_xfer(dev->bus, dev->addr, out, sizeof(out), NULL, 0);
}

static int32_t i2c_hid_field_get(const uint8_t *report, size_t len,
                                 const i2c_hid_field_t *field, bool is_signed) {
    uint32_t val = 0;
    for (uint8_t i = 0; i < field->size; i++) {
        size_t bit = (size_t)field->bit + i;
        if (bit / 8 >= len) break;
        val |= (uint32_t)((report[bit / 8] >> (bit % 8)) & 1) << i;
    }

    if (is_signed && field->size && field->size < 32 && (val & (1u << (field->size - 1))))
        val |= ~0u << field->size;
    return (int32_t)val;
}

// called for every input field inside the mouse collection, keeps the ones a mouse event needs
static void i2c_hid_take_field(i2c_hid_dev_t *dev, uint8_t id, uint16_t page, uint16_t usage,
                               bool relative, i2c_hid_field_t field) {
    if (page == HID_PAGE_BUTTON) {
        // buttons 1-5 are consecutive bits in the same order mouse_event_t keeps them
        if (field.size != 1 || usage < 1 || usage > 5) return;

        if (usage == 1 && !dev->buttons.size)
            dev->buttons = field;
        else if (usage == dev->buttons.size + 1 && field.bit == dev->buttons.bit + dev->buttons.size)
            dev->buttons.size++;
        return;
    }

    // absolute x/y would be the touch surface, thats a different collection anyway
    if (page != HID_PAGE_DESKTOP || !relative || field.size > 16) return;

    i2c_hid_field_t *slot = NULL;
    if (usage == HID_USAGE_X)     slot = &dev->x;
    if (usage == HID_USAGE_Y)     slot = &dev->y;
    if (usage == HID_USAGE_WHEEL) slot = &dev->wheel;
    if (!slot || slot->size) return;

    *slot = field;
    if (usage == HID_USAGE_X)
        dev->mouse_id = id;
}

// find the mouse collection and remember where its fields sit, the multitouch
// one a precision touchpad also has stays quiet until its asked for
static bool i2c_hid_parse(i2c_hid_dev_t *dev, const uint8_t *rd, size_t len) {
    static uint16_t bits[256];      // how far into each report id the fields have got
    memset(bits, 0, sizeof(bits));

    uint16_t usages[I2C_HID_MAX_USAGES];
    uint32_t usage_count = 0;
    uint32_t usage_min   = 0;
    uint32_t usage_max   = 0;

    uint16_t page  = 0;
    uint32_t size  = 0;
    uint32_t count = 0;
    uint8_t  id    = 0;

    int depth       = 0;
    int mouse_depth = 0;    // depth the mouse collection opened at, 0 while outside it

    for (size_t off = 0; off < len; ) {
        uint8_t prefix = rd[off++];

        if (prefix == HID_ITEM_LONG) {
            if (off >= len) break;
            off += (size_t)rd[off] + 2;
            continue;
        }

        // a size of 3 means 4 bytes
        uint8_t n = (prefix & 3) == 3 ? 4 : (prefix & 3);
        if (off + n > len) break;

        uint32_t data = 0;
        for (uint8_t i = 0; i < n; i++)
            data |= (uint32_t)rd[off + i] << (i * 8);
        off += n;

        switch (prefix & HID_ITEM_TAG_MASK) {
            case HID_ITEM_USAGE_PAGE:   page  = (uint16_t)data; break;
            case HID_ITEM_REPORT_SIZE:  size  = data;           break;
            case HID_ITEM_REPORT_COUNT: count = data;           break;
            case HID_ITEM_USAGE_MIN:    usage_min = data;       break;
            case HID_ITEM_USAGE_MAX:    usage_max = data;       break;

            case HID_ITEM_REPORT_ID:
                id = (uint8_t)data;
                dev->has_report_ids = true;
                break;

            case HID_ITEM_USAGE:
                if (usage_count < I2C_HID_MAX_USAGES)
                    usages[usage_count++] = (uint16_t)data;
                break;

            case HID_ITEM_COLLECTION:
                depth++;
                if (data == HID_COLLECTION_APP && !mouse_depth && page == HID_PAGE_DESKTOP &&
                    usage_count && usages[0] == HID_USAGE_MOUSE)
                    mouse_depth = depth;
                break;

            case HID_ITEM_END_COLLECTION:
                if (depth == mouse_depth) mouse_depth = 0;
                if (depth) depth--;
                break;

            case HID_ITEM_INPUT:
                if (size > 32 || count > 256) break;

                for (uint32_t i = 0; mouse_depth && !(data & HID_INPUT_CONSTANT) && i < count; i++) {
                    // short of usages the last one covers every field thats left
                    uint32_t usage = usage_min + i < usage_max ? usage_min + i : usage_max;
                    if (usage_count)
                        usage = usages[i < usage_count ? i : usage_count - 1];

                    i2c_hid_field_t field = {
                        .bit  = (uint16_t)(bits[id] + i * size),
                        .size = (uint8_t)size,
                    };
                    i2c_hid_take_field(dev, id, page, (uint16_t)usage,
                                       data & HID_INPUT_RELATIVE, field);
                }
                bits[id] += (uint16_t)(size * count);
                break;

            default:
                break;
        }

        // usages only last until the next main item
        if ((prefix & HID_ITEM_TYPE_MASK) == 0) {
            usage_count = 0;
            usage_min   = 0;
            usage_max   = 0;
        }
    }

    return dev->x.size && dev->y.size;
}

bool i2c_hid_poll(void) {
    i2c_hid_dev_t *dev = &trackpad;
    if (!dev->present) return false;

    uint8_t buf[I2C_HID_MAX_INPUT];
    int got = dw_i2c_read_prefixed(dev->bus, dev->addr, buf, dev->max_input);
    if (got < 0) {
        if (++dev->errors >= I2C_HID_MAX_ERRORS) {
            serial_printf(LOG_WARN "i2chid: trackpad stopped answering, giving up on it\n");
            dev->present = false;
        }
        return false;
    }
    dev->errors = 0;

    // nothing to say reads back as a length of 0
    size_t len = buf[0] | ((size_t)buf[1] << 8);
    if (len <= 2 || len > (size_t)got) return false;

    const uint8_t *report = buf + 2;
    len -= 2;

    if (dev->has_report_ids) {
        if (report[0] != dev->mouse_id) return true;
        report++;
        len--;
    }

    int32_t wheel = i2c_hid_field_get(report, len, &dev->wheel, true);
    if (wheel >  127) wheel =  127;
    if (wheel < -127) wheel = -127;

    // hid counts +y as down the screen same as usb, no flip here either
    mouse_event_t ev = {
        .dx      = (int16_t)i2c_hid_field_get(report, len, &dev->x, true),
        .dy      = (int16_t)i2c_hid_field_get(report, len, &dev->y, true),
        .wheel   = (int8_t)wheel,
        .buttons = (uint8_t)i2c_hid_field_get(report, len, &dev->buttons, false)
    };

    mouse_input_dispatch(&ev);
    return true;
}

// power it up, reset it and work out what its reports look like, and wake sleeping devices
static bool i2c_hid_start(i2c_hid_dev_t *dev) {
    if (i2c_hid_command(dev, I2C_HID_CMD_SET_POWER, I2C_HID_POWER_ON) < 0 &&
        i2c_hid_command(dev, I2C_HID_CMD_SET_POWER, I2C_HID_POWER_ON) < 0) {
        serial_printf(LOG_WARN "i2chid: SET_POWER failed\n");
        return false;
    }
    wait_ms(I2C_HID_POWER_ON_MS);

    if (i2c_hid_command(dev, I2C_HID_CMD_RESET, 0) < 0) {
        serial_printf(LOG_WARN "i2chid: RESET failed\n");
        return false;
    }

    // its back once it hands over an empty report
    uint8_t ack[3];
    bool    back = false;
    for (int i = 0; i < I2C_HID_RESET_TRIES && !back; i++) {
        wait_ms(I2C_HID_RESET_POLL_MS);
        back = dw_i2c_read_prefixed(dev->bus, dev->addr, ack, sizeof(ack)) >= 2 &&
               !ack[0] && !ack[1];
    }
    if (!back)
        serial_printf(LOG_WARN "i2chid: reset never got answered, carrying on anyway\n");

    uint16_t rd_len = dev->desc.report_desc_length;
    if (rd_len == 0 || rd_len > I2C_HID_MAX_REPORT_DESC || dev->desc.max_input_length < 3) {
        serial_printf(LOG_WARN "i2chid: HID descriptor makes no sense (report desc %u, input %u)\n",
                      rd_len, dev->desc.max_input_length);
        return false;
    }

    uint8_t *rd = kmalloc(rd_len);
    if (!rd) return false;

    if (i2c_hid_read_reg(dev->bus, dev->addr, dev->desc.report_desc_reg, rd, rd_len) != rd_len) {
        serial_printf(LOG_WARN "i2chid: cant read the report descriptor\n");
        kfree(rd);
        return false;
    }

    bool mouse = i2c_hid_parse(dev, rd, rd_len);
    kfree(rd);
    if (!mouse) {
        serial_printf(LOG_INFO "i2chid: no mouse collection in there, not a trackpad\n");
        return false;
    }

    dev->max_input = dev->desc.max_input_length;
    if (dev->max_input > I2C_HID_MAX_INPUT)
        dev->max_input = I2C_HID_MAX_INPUT;

    return true;
}

bool i2c_hid_probe(dw_i2c_t *bus, int bus_index, uint8_t addr) {
    i2c_hid_dev_t *dev = &trackpad;

    for (size_t r = 0; r < sizeof(hid_desc_regs) / sizeof(hid_desc_regs[0]); r++) {
        i2c_hid_desc_t desc;
        if (i2c_hid_read_reg(bus, addr, hid_desc_regs[r], &desc, sizeof(desc)) != (int)sizeof(desc))
            return false;

        // the wrong register still answers, just not with one of these
        if (desc.length != I2C_HID_DESC_LENGTH || desc.version != I2C_HID_VERSION) {
            serial_printf(LOG_INFO "i2chid: bus %d 0x%02x acked, 0x%04x isnt a HID descriptor "
                          "(length=%u version=0x%04x)\n", bus_index, addr,
                          hid_desc_regs[r], desc.length, desc.version);
            continue;
        }

        memset(dev, 0, sizeof(*dev));
        dev->bus  = bus;
        dev->addr = addr;
        dev->desc = desc;

        serial_printf(LOG_INFO "i2chid: device %04x:%04x on bus %d at 0x%02x, descriptor at 0x%04x\n",
                      desc.vendor_id, desc.product_id, bus_index, addr, hid_desc_regs[r]);

        if (!i2c_hid_start(dev))
            return false;

        dev->present = true;
        serial_printf(LOG_OK "i2chid: trackpad ready, mouse report id %u, %u button(s)%s\n",
                      dev->mouse_id, dev->buttons.size, dev->wheel.size ? ", wheel" : "");
        return true;
    }
    return false;
}
