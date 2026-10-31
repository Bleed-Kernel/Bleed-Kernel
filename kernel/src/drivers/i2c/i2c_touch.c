#include <drivers/i2c/i2c_touch.h>
#include <drivers/i2c/i2c_hid.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <mm/spinlock.h>
#include <ansii.h>
#include <stdio.h>

static i2c_hid_dev_t *screens[I2C_TOUCH_MAX];
static size_t         screen_count = 0;

bool i2c_touch_add(i2c_hid_dev_t *dev) {
    if (screen_count >= I2C_TOUCH_MAX) {
        serial_printf(LOG_WARN "i2c: already have %u touchscreens, leaving %04x:%04x alone\n",
                      I2C_TOUCH_MAX, dev->desc.vendor_id, dev->desc.product_id);
        return false;
    }

    screens[screen_count++] = dev;
    serial_printf(LOG_OK "i2c: touchscreen %04x:%04x at 0x%02x ready, %u finger(s)\n",
                  dev->desc.vendor_id, dev->desc.product_id, dev->addr, dev->hid.finger_count);
    return true;
}

bool i2c_touch_present(void) {
    return screen_count != 0;
}

void i2c_touch_watch(void) {
    uint32_t quiet = 0;

    for (;;) {
        uint64_t      deadline = sched_deadline_ms(quiet < I2C_TOUCH_IDLE_AFTER ? I2C_TOUCH_POLL_ACTIVE_MS
                                                                                 : I2C_TOUCH_POLL_IDLE_MS);
        unsigned long flags    = irq_push();
        sched_sleep_until(deadline);
        irq_restore(flags);

        bool active = false;
        for (size_t s = 0; s < screen_count; s++) {
            for (int r = 0; r < I2C_TOUCH_BURST && i2c_hid_poll(screens[s]); r++)
                active = true;
        }

        if (active)
            quiet = 0;
        else if (quiet < I2C_TOUCH_IDLE_AFTER)
            quiet++;
    }
}
