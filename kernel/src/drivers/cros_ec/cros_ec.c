#include <drivers/cros_ec/cros_ec.h>
#include <drivers/serial/serial.h>
#include <cpu/io.h>
#include <ansii.h>

static bool ec_found = false;

// memmap helpers
static inline uint8_t ec_read8(uint16_t offset) {
    return inb(CROS_EC_MEMMAP + offset);
}

static inline uint32_t ec_read32(uint16_t offset) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value |= (uint32_t)ec_read8(offset + i) << (i * 8);
    return value;
}

void cros_ec_init(void) {
    // a port nothing answers on reads back 0xFF
    ec_found = ec_read8(CROS_EC_MEMMAP_ID) == 'E' && ec_read8(CROS_EC_MEMMAP_ID + 1) == 'C';
    if (!ec_found) {
        serial_printf(LOG_INFO "cros_ec: no ChromeOS EC found, no battery to read\n");
        return;
    }

    cros_ec_battery_t batt;
    if (!cros_ec_battery(&batt)) {
        serial_printf(LOG_WARN "cros_ec: EC found but it has no battery to report (flags 0x%x)\n",
                      ec_read8(CROS_EC_BATT_FLAG));
        return;
    }
    serial_printf(LOG_OK "cros_ec: battery at %u%% (%u/%u mAh, %u mV), %s\n", batt.percent, batt.capacity,
                  batt.full, batt.voltage, batt.charging ? "charging" : batt.ac ? "on AC" : "discharging");
}

bool cros_ec_present(void) {
    return ec_found;
}

bool cros_ec_battery(cros_ec_battery_t *out) {
    if (!ec_found || !out) return false;

    uint8_t flags = ec_read8(CROS_EC_BATT_FLAG);
    if (!(flags & CROS_EC_FLAG_PRESENT) || (flags & CROS_EC_FLAG_INVALID)) return false;

    uint32_t cap  = ec_read32(CROS_EC_BATT_CAP);
    uint32_t full = ec_read32(CROS_EC_BATT_LFCC);
    if (!full || full == 0xFFFFFFFF) return false;

    // a battery thats just been topped up can hold more than it last called full
    uint32_t percent = (uint32_t)((uint64_t)cap * 100 / full);
    if (percent > 100) percent = 100;

    out->ac       = flags & CROS_EC_FLAG_AC;
    out->charging = flags & CROS_EC_FLAG_CHARGE;
    out->percent  = (uint8_t)percent;
    out->voltage  = ec_read32(CROS_EC_BATT_VOLT);
    out->rate     = ec_read32(CROS_EC_BATT_RATE);
    out->capacity = cap;
    out->full     = full;
    return true;
}
