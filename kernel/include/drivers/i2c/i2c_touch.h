#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <drivers/i2c/i2c_hid.h>

#define I2C_TOUCH_MAX               2
#define I2C_TOUCH_POLL_ACTIVE_MS    8
#define I2C_TOUCH_POLL_IDLE_MS      30
#define I2C_TOUCH_IDLE_AFTER        64
#define I2C_TOUCH_BURST             10

// addresses touchscreens sit at, for firmware that hides its whole _CRS from us.
static const uint8_t touch_guess_addrs[] = { 0x10, 0x14, 0x5D };

// Take a HID device that has a touchscreen in it, false if theres no room
bool i2c_touch_add(i2c_hid_dev_t *dev);

// true if theres at least one, no point starting the task otherwise
bool i2c_touch_present(void);

// entry point of the touchscreen task, polls every screen. nothing drives the
// gpio pins they interrupt on so theres nothing to sleep on
void i2c_touch_watch(void);
