#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/hid/hid.h>

#define I2C_HID_DESC_LENGTH     30
#define I2C_HID_VERSION         0x0100
#define I2C_HID_MAX_INPUT       256
#define I2C_HID_MAX_DEVICES     4

// command register opcodes
#define I2C_HID_CMD_RESET       0x01
#define I2C_HID_CMD_SET_REPORT  0x03
#define I2C_HID_CMD_SET_POWER   0x08
#define I2C_HID_REPORT_FEATURE  0x03
#define I2C_HID_REPORT_ID_ESC   0x0F
#define I2C_HID_POWER_ON        0x00

#define I2C_HID_POWER_ON_MS     60      // listen im eyeballing this, 
#define I2C_HID_RESET_POLL_MS   10
#define I2C_HID_RESET_TRIES     50

#define I2C_HID_MAX_ERRORS      16      // strikes before we decide the device is gone

// where the HID descriptor lives is in a _DSM we cant run, its nearly always one of these.
static const uint16_t hid_desc_regs[] = { 0x0001, 0x0020, 0x0000 };

// the HID descriptor, says where every other register is
typedef struct __attribute__((packed)) i2c_hid_desc {
    uint16_t length;
    uint16_t version;
    uint16_t report_desc_length;
    uint16_t report_desc_reg;
    uint16_t input_reg;
    uint16_t max_input_length;
    uint16_t output_reg;
    uint16_t max_output_length;
    uint16_t command_reg;
    uint16_t data_reg;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version_id;
    uint32_t reserved;
} i2c_hid_desc_t;

typedef struct i2c_hid_dev {
    bool            present;
    dw_i2c_t       *bus;
    uint8_t         addr;
    i2c_hid_desc_t  desc;
    uint16_t        max_input;
    uint8_t         errors;
    hid_input_t     hid;        // where its reports keep things
} i2c_hid_dev_t;

/// @brief See if the device at addr speaks HID over I2C, power it up and work out its reports
/// @return the device, or NULL if it isnt HID or has nothing we know what to do with
i2c_hid_dev_t *i2c_hid_probe(dw_i2c_t *bus, int bus_index, uint8_t addr);

// Nobody wanted it after all, its slot goes back
void i2c_hid_forget(i2c_hid_dev_t *dev);

// Read one report and pass it on, true if there was one waiting
bool i2c_hid_poll(i2c_hid_dev_t *dev);
