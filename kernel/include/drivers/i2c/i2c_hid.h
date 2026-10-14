#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/i2c/dw_i2c.h>

#define I2C_HID_DESC_LENGTH     30
#define I2C_HID_VERSION         0x0100
#define I2C_HID_MAX_REPORT_DESC 4096    // biggest report descriptor we bother reading
#define I2C_HID_MAX_INPUT       256
#define I2C_HID_MAX_USAGES      16

// command register opcodes
#define I2C_HID_CMD_RESET       0x01
#define I2C_HID_CMD_SET_POWER   0x08
#define I2C_HID_POWER_ON        0x00

#define I2C_HID_POWER_ON_MS     60      // listen im eyeballing this, 
#define I2C_HID_RESET_POLL_MS   10
#define I2C_HID_RESET_TRIES     50

#define I2C_HID_MAX_ERRORS      16      // strikes before we decide the trackpad is gone

// report descriptor items, the low 2 bits of the prefix are the data size so mask them off
#define HID_ITEM_TAG_MASK       0xFC
#define HID_ITEM_TYPE_MASK      0x0C    // 0 = main
#define HID_ITEM_LONG           0xFE

#define HID_ITEM_INPUT          0x80
#define HID_ITEM_COLLECTION     0xA0
#define HID_ITEM_END_COLLECTION 0xC0
#define HID_ITEM_USAGE_PAGE     0x04
#define HID_ITEM_REPORT_SIZE    0x74
#define HID_ITEM_REPORT_ID      0x84
#define HID_ITEM_REPORT_COUNT   0x94
#define HID_ITEM_USAGE          0x08
#define HID_ITEM_USAGE_MIN      0x18
#define HID_ITEM_USAGE_MAX      0x28

#define HID_INPUT_CONSTANT      (1u << 0)   // padding
#define HID_INPUT_RELATIVE      (1u << 2)
#define HID_COLLECTION_APP      0x01

#define HID_PAGE_DESKTOP        0x01
#define HID_PAGE_BUTTON         0x09
#define HID_USAGE_MOUSE         0x02
#define HID_USAGE_X             0x30
#define HID_USAGE_Y             0x31
#define HID_USAGE_WHEEL         0x38

// where the HID descriptor lives is in a _DSM we cant run, its nearly always one of these
static const uint16_t hid_desc_regs[] = { 0x0001, 0x0020 };

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

typedef struct i2c_hid_field {
    uint16_t bit;       // offset into the report, the id byte not counted
    uint8_t  size;      // in bits, 0 = the report doesnt have one
} i2c_hid_field_t;

typedef struct i2c_hid_dev {
    bool            present;
    dw_i2c_t       *bus;
    uint8_t         addr;
    i2c_hid_desc_t  desc;
    uint16_t        max_input;
    uint8_t         errors;

    // where the mouse report keeps things, worked out from the report descriptor
    bool            has_report_ids;
    uint8_t         mouse_id;
    i2c_hid_field_t buttons;
    i2c_hid_field_t x;
    i2c_hid_field_t y;
    i2c_hid_field_t wheel;
} i2c_hid_dev_t;

/// @brief See if the device at addr speaks HID over I2C and has a mouse in it
/// @return true if its now the trackpad i2c_hid_poll reads
bool i2c_hid_probe(dw_i2c_t *bus, int bus_index, uint8_t addr);

// Read one report and pass it on as a mouse event, true if there was one waiting
bool i2c_hid_poll(void);
