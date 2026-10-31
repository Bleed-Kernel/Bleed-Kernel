#include <drivers/i2c/i2c_hid.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/hid/hid.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static i2c_hid_dev_t devices[I2C_HID_MAX_DEVICES];

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

// the command says which feature report, the report itself goes through the data register
static int i2c_hid_set_feature(i2c_hid_dev_t *dev, uint8_t id, const uint8_t *data, size_t len) {
    uint16_t cmd_reg  = dev->desc.command_reg;
    uint16_t data_reg = dev->desc.data_reg;
    uint8_t  out[16 + HID_MAX_FEATURE];
    size_t   n = 0;

    if (len > HID_MAX_FEATURE) return -1;

    out[n++] = (uint8_t)cmd_reg;
    out[n++] = (uint8_t)(cmd_reg >> 8);
    out[n++] = (uint8_t)((I2C_HID_REPORT_FEATURE << 4) | (id < I2C_HID_REPORT_ID_ESC ? id : I2C_HID_REPORT_ID_ESC));
    out[n++] = I2C_HID_CMD_SET_REPORT;
    if (id >= I2C_HID_REPORT_ID_ESC)
        out[n++] = id;

    // the length counts itself
    size_t total = 2 + (id ? 1 : 0) + len;
    out[n++] = (uint8_t)data_reg;
    out[n++] = (uint8_t)(data_reg >> 8);
    out[n++] = (uint8_t)total;
    out[n++] = (uint8_t)(total >> 8);
    if (id)
        out[n++] = id;
    memcpy(&out[n], data, len);
    n += len;

    return dw_i2c_xfer(dev->bus, dev->addr, out, n, NULL, 0);
}

// out of mouse mode, read without the interrupt it hands the last report back and deltas etc
static void i2c_hid_touchpad_mode(i2c_hid_dev_t *dev) {
    uint8_t mode[HID_MAX_FEATURE];
    size_t  len = hid_touchpad_mode(&dev->hid, mode, sizeof(mode));

    if (!len || i2c_hid_set_feature(dev, dev->hid.input_mode_id, mode, len) < 0) {
        serial_printf(LOG_WARN "i2chid: %04x:%04x wont switch to touchpad mode, leaving it a mouse\n",
                      dev->desc.vendor_id, dev->desc.product_id);
        dev->hid.has_touchpad = false;
        return;
    }
    serial_printf(LOG_INFO "i2chid: %04x:%04x switched to touchpad mode\n",
                  dev->desc.vendor_id, dev->desc.product_id);
}

bool i2c_hid_poll(i2c_hid_dev_t *dev) {
    if (!dev || !dev->present) return false;

    uint8_t buf[I2C_HID_MAX_INPUT];
    int got = dw_i2c_read_prefixed(dev->bus, dev->addr, buf, dev->max_input);
    if (got < 0) {
        if (++dev->errors >= I2C_HID_MAX_ERRORS) {
            serial_printf(LOG_WARN "i2chid: %04x:%04x stopped answering, giving up on it\n",
                          dev->desc.vendor_id, dev->desc.product_id);
            hid_release(&dev->hid);
            dev->present = false;
        }
        return false;
    }
    dev->errors = 0;

    // nothing to say reads back as a length of 0
    size_t len = buf[0] | ((size_t)buf[1] << 8);
    if (len <= 2 || len > (size_t)got) return false;

    hid_report(&dev->hid, buf + 2, len - 2);
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
    if (rd_len == 0 || rd_len > HID_MAX_REPORT_DESC || dev->desc.max_input_length < 3) {
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

    bool known = hid_parse(&dev->hid, rd, rd_len);
    kfree(rd);
    hid_describe(&dev->hid, "i2chid");
    if (!known) return false;

    if (dev->hid.has_touchpad)
        i2c_hid_touchpad_mode(dev);

    dev->max_input = dev->desc.max_input_length;
    if (dev->max_input > I2C_HID_MAX_INPUT)
        dev->max_input = I2C_HID_MAX_INPUT;

    return true;
}

i2c_hid_dev_t *i2c_hid_probe(dw_i2c_t *bus, int bus_index, uint8_t addr) {
    i2c_hid_dev_t *dev = NULL;
    for (int i = 0; i < I2C_HID_MAX_DEVICES && !dev; i++) {
        if (!devices[i].present)
            dev = &devices[i];
    }
    if (!dev) {
        serial_printf(LOG_WARN "i2chid: no room for another device at 0x%02x\n", addr);
        return NULL;
    }

    for (size_t r = 0; r < sizeof(hid_desc_regs) / sizeof(hid_desc_regs[0]); r++) {
        i2c_hid_desc_t desc;
        if (i2c_hid_read_reg(bus, addr, hid_desc_regs[r], &desc, sizeof(desc)) != (int)sizeof(desc))
            return NULL;

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
            return NULL;

        dev->present = true;
        return dev;
    }
    return NULL;
}

void i2c_hid_forget(i2c_hid_dev_t *dev) {
    if (dev) dev->present = false;
}
