#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <drivers/i2c/dw_i2c.h>
#include <input/trackpad_gesture.h>

/*
    Elan's own trackpad protocol, Imagine my surprise when I find out the
    protocol I had been using actually wouldnt work on the only i2c hardware I have
    this is for the chromie
    anyways documentation for this elan trackpad really sucks everything is in the linux source
    I have provided
    https://github.com/torvalds/linux/blob/master/drivers/input/mouse/elan_i2c_i2c.c
*/

// registers, 16 bits low byte first
#define ELAN_REG_DESC           0x0001
#define ELAN_REG_REPORT_DESC    0x0002
#define ELAN_REG_STAND          0x0005
#define ELAN_REG_MAX_X          0x0106
#define ELAN_REG_MAX_Y          0x0107
#define ELAN_REG_SET            0x0300

#define ELAN_STAND_RESET        0x0100
#define ELAN_STAND_WAKE_UP      0x0800
#define ELAN_SET_ABSOLUTE       0x0001  // finger positions instead of mouse deltas

#define ELAN_DESC_LENGTH        30
#define ELAN_REPORT_DESC_LENGTH 158
#define ELAN_REPORT_LENGTH      34
#define ELAN_RESET_MS           100

// absolute report layout
#define ELAN_REPORT_ID_OFFSET   2
#define ELAN_TOUCH_INFO_OFFSET  3       // buttons in bits 0-2, a bit per finger from bit 3
#define ELAN_FINGER_OFFSET      4
#define ELAN_FINGER_LENGTH      5       // only fingers that are down get one
#define ELAN_MAX_FINGERS        5
#define ELAN_REPORT_ID          0x5D
#define ELAN_REPORT_ID_HIRES    0x60    // newer pads, 16 bit positions

#define ELAN_INFO_LEFT          (1 << 0)
#define ELAN_INFO_RIGHT         (1 << 1)
#define ELAN_INFO_MIDDLE        (1 << 2)

#define ELAN_MAX_ERRORS         16

typedef struct elan_dev {
    bool      present;
    dw_i2c_t *bus;
    uint8_t   addr;
    uint8_t   errors;
    uint16_t  max_x;
    uint16_t  max_y;

    trackpad_gesture_t pad;
} elan_dev_t;

/// @brief See if the device at addr speaks the Elan protocol and put it in absolute mode
/// @return true if its now the trackpad elan_i2c_poll reads
bool elan_i2c_probe(dw_i2c_t *bus, int bus_index, uint8_t addr);

// Read one report and turn it into mouse events, true if a finger or button is down
bool elan_i2c_poll(void);
