#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CROS_EC_MEMMAP          0x900       // io port of the block the ec keeps up to date itself
#define CROS_EC_MEMMAP_ID       0x20        // reads 'E' 'C' if theres one there
#define CROS_EC_BATT_VOLT       0x40        // mV
#define CROS_EC_BATT_RATE       0x44        // mA
#define CROS_EC_BATT_CAP        0x48        // mAh left
#define CROS_EC_BATT_FLAG       0x4C
#define CROS_EC_BATT_LFCC       0x58        // mAh it held when it was last full

#define CROS_EC_FLAG_AC         (1 << 0)
#define CROS_EC_FLAG_PRESENT    (1 << 1)
#define CROS_EC_FLAG_DISCHARGE  (1 << 2)
#define CROS_EC_FLAG_CHARGE     (1 << 3)
#define CROS_EC_FLAG_INVALID    (1 << 5)    // the ec hasnt heard from the battery yet

typedef struct cros_ec_battery {
    bool     ac;
    bool     charging;
    uint8_t  percent;
    uint32_t voltage;       // mV
    uint32_t rate;          // mA
    uint32_t capacity;      // mAh left
    uint32_t full;          // mAh
} cros_ec_battery_t;

// look for a chromebook embedded controller, nothing else has one
void cros_ec_init(void);

bool cros_ec_present(void);

// false if theres no ec, no battery in it or the ec has nothing to say about it yet
bool cros_ec_battery(cros_ec_battery_t *out);
