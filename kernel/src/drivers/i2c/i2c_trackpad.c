#include <drivers/i2c/i2c_trackpad.h>
#include <drivers/i2c/dw_i2c.h>
#include <drivers/i2c/i2c_hid.h>
#include <drivers/i2c/elan_i2c.h>
#include <drivers/i2c/i2c_touch.h>
#include <drivers/serial/serial.h>
#include <ACPI/acpi_i2c.h>
#include <ACPI/acpi_apic.h>
#include <sched/scheduler.h>
#include <mm/spinlock.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

static acpi_i2c_dev_t acpi_devs[ACPI_I2C_MAX_DEVICES];
static size_t         acpi_dev_count = 0;
static bool           acpi_done[ACPI_I2C_MAX_DEVICES];   // answered on some bus already

// whichever protocol claimed the trackpad
static bool (*trackpad_poll)(void) = NULL;

// 0 = polled
static uint32_t      trackpad_gsi = 0;
static bool          irq_level    = false;
static bool          irq_low      = false;
static volatile bool irq_pending  = false;
static wait_queue_t  trackpad_queue;

// hid ones go through here so a touchscreen goes to the touch side instead
static i2c_hid_dev_t *trackpad_hid = NULL;

static bool trackpad_hid_poll(void) {
    return i2c_hid_poll(trackpad_hid);
}

// hand a HID device to whoever wants it, false if nobody did and its been let go
static bool i2c_hid_claim(i2c_hid_dev_t *hid) {
    if (hid->hid.has_touch) {
        if (i2c_touch_add(hid))
            return true;
    } else if ((hid->hid.has_mouse || hid->hid.has_touchpad) && !trackpad_poll) {
        trackpad_hid  = hid;
        trackpad_poll = trackpad_hid_poll;
        return true;
    }

    i2c_hid_forget(hid);
    return false;
}

static void i2c_trackpad_use_irq(const acpi_i2c_dev_t *dev) {
    if (dev->irq >= I2C_TRACKPAD_MIN_GSI) {
        trackpad_gsi = dev->irq;
        irq_level    = dev->irq_level;
        irq_low      = dev->irq_low;
        serial_printf(LOG_INFO "i2c: trackpad interrupts on gsi %u (%s, active %s)\n", dev->irq,
                      irq_level ? "level" : "edge", irq_low ? "low" : "high");
    } else if (dev->gpio_irq) {
        serial_printf(LOG_INFO "i2c: trackpad interrupt is gpio pin %u, nothing here drives "
                      "those so its polled\n", dev->gpio_pin);
    } else {
        serial_printf(LOG_INFO "i2c: firmware doesnt say how the trackpad interrupts, polling it\n");
    }
}

// PNP0C50 is the generic one, an ELAN id without it is their own protocol.
// plenty of elan pads answer to both so the other one is worth a go if the first says no.
// the firmware doesnt say which bus so each device gets tried on every one until it answers
static void i2c_probe_acpi(dw_i2c_t *bus, int bus_index) {
    for (size_t i = 0; i < acpi_dev_count; i++) {
        acpi_i2c_dev_t *dev  = &acpi_devs[i];
        uint8_t         addr = (uint8_t)dev->addr;
        bool            elan = !strncmp(dev->hid, "ELAN", 4);

        if (acpi_done[i] || (!dev->hid_over_i2c && !elan)) continue;

        i2c_hid_dev_t *hid = NULL;
        if (dev->hid_over_i2c)
            hid = i2c_hid_probe(bus, bus_index, addr);
        if (!hid && elan && !trackpad_poll && elan_i2c_probe(bus, bus_index, addr)) {
            acpi_done[i]  = true;
            trackpad_poll = elan_i2c_poll;
            i2c_trackpad_use_irq(dev);
            continue;
        }
        if (!hid && elan && !dev->hid_over_i2c)
            hid = i2c_hid_probe(bus, bus_index, addr);
        if (!hid) continue;

        acpi_done[i] = true;
        if (i2c_hid_claim(hid) && trackpad_hid == hid)
            i2c_trackpad_use_irq(dev);
    }
}

// never knock on an address the firmware says is something else, a 2 byte
// write means something very different to an audio codec
static void i2c_probe_guess(dw_i2c_t *bus, int bus_index, const uint8_t *addrs, size_t count) {
    for (size_t a = 0; a < count; a++) {
        bool taken = false;
        for (size_t i = 0; i < acpi_dev_count; i++) {
            // one we couldnt name is most likely the very thing were guessing at
            if (acpi_devs[i].addr == addrs[a] && acpi_devs[i].hid[0])
                taken = true;
        }
        if (taken) continue;

        i2c_hid_dev_t *hid = i2c_hid_probe(bus, bus_index, addrs[a]);
        if (hid)
            i2c_hid_claim(hid);
    }
}

bool i2c_trackpad_present(void) {
    return trackpad_poll != NULL;
}

void i2c_trackpad_interrupt(void) {
    if (!trackpad_gsi) return;

    // a level one stays up until the pad is read, which cant be done from in here
    if (irq_level)
        ioapic_mask_gsi(trackpad_gsi);

    irq_pending = true;
    sched_wake_queue(&trackpad_queue);
}

// also what lets a level interrupt back in after the read
static bool trackpad_irq_arm(void) {
    unsigned long flags = irq_push();
    int routed = ioapic_route_gsi(trackpad_gsi, 32 + I2C_TRACKPAD_IRQ, irq_level, irq_low);
    irq_restore(flags);
    return routed == 0;
}

static void trackpad_irq_give_up(const char *why) {
    unsigned long flags = irq_push();
    ioapic_mask_gsi(trackpad_gsi);
    trackpad_gsi = 0;
    irq_restore(flags);

    serial_printf(LOG_WARN "i2c: %s, polling the trackpad instead\n", why);
}

// true if the pad interrupted, false if ms went by first
static bool trackpad_wait(uint32_t ms) {
    uint64_t      deadline = sched_deadline_ms(ms);
    unsigned long flags    = irq_push();

    while (!irq_pending && !sched_deadline_passed(deadline))
        sched_wait_queue(&trackpad_queue, deadline);

    bool fired  = irq_pending;
    irq_pending = false;
    irq_restore(flags);
    return fired;
}

void i2c_trackpad_watch(void) {
    uint32_t quiet  = 0;
    uint32_t missed = 0;
    uint32_t burst  = 0;
    uint64_t window = 0;
    bool     seen   = false;

    if (trackpad_gsi && !trackpad_irq_arm())
        trackpad_irq_give_up("the ioapic has no pin for the trackpad interrupt");

    for (;;) {
        if (!trackpad_gsi) {
            // nothing tells us when a finger comes back, so it only slows down, never stops
            trackpad_wait(quiet < I2C_TRACKPAD_IDLE_AFTER ? I2C_TRACKPAD_POLL_ACTIVE_MS
                                                          : I2C_TRACKPAD_POLL_IDLE_MS);
            if (trackpad_poll())
                quiet = 0;
            else if (quiet < I2C_TRACKPAD_IDLE_AFTER)
                quiet++;
            continue;
        }

        bool fired  = trackpad_wait(I2C_TRACKPAD_WATCHDOG_MS);
        bool active = trackpad_poll();
        if (irq_level)
            trackpad_irq_arm();

        if (sched_deadline_passed(window)) {
            window = sched_deadline_ms(1000);
            burst  = 0;
        }

        if (fired) {
            seen = true;
            if (++burst > I2C_TRACKPAD_IRQ_STORM)
                trackpad_irq_give_up("the trackpad interrupt wont stop firing");
        } else if (active && !seen && ++missed >= I2C_TRACKPAD_IRQ_MISSES) {
            // one that has worked once is trusted, the watchdog read is what frees a stuck edge
            trackpad_irq_give_up("the trackpad interrupt never fires");
        }
    }
}

void i2c_trackpad_init(void) {
    trackpad_poll = NULL;
    trackpad_hid  = NULL;
    trackpad_gsi  = 0;
    if (!dw_i2c_get_bus(0)) return;

    acpi_dev_count = acpi_i2c_devices(acpi_devs, ACPI_I2C_MAX_DEVICES);
    for (size_t i = 0; i < acpi_dev_count; i++) {
        acpi_done[i] = false;
        serial_printf(LOG_INFO "i2c: firmware lists %s at 0x%02x%s\n",
                      acpi_devs[i].hid[0] ? acpi_devs[i].hid : "something",
                      acpi_devs[i].addr, acpi_devs[i].hid_over_i2c ? " (HID over I2C)" : "");
    }

    dw_i2c_t *bus;
    for (int n = 0; (bus = dw_i2c_get_bus(n)); n++)
        i2c_probe_acpi(bus, n);

    for (int n = 0; !trackpad_poll && (bus = dw_i2c_get_bus(n)); n++)
        i2c_probe_guess(bus, n, guess_addrs, sizeof(guess_addrs));

    // only when the firmware shows us nothing at all, theres too much else living down there
    for (int n = 0; !acpi_dev_count && !i2c_touch_present() && (bus = dw_i2c_get_bus(n)); n++)
        i2c_probe_guess(bus, n, touch_guess_addrs, sizeof(touch_guess_addrs));

    if (!trackpad_poll)
        serial_printf(LOG_INFO "i2c: no trackpad on any bus\n");
    if (!i2c_touch_present())
        serial_printf(LOG_INFO "i2c: no touchscreen on any bus\n");
}
