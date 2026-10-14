#pragma once

#include <stdint.h>
#include <stdbool.h>

#define I2C_TRACKPAD_POLL_ACTIVE_MS 8
#define I2C_TRACKPAD_POLL_IDLE_MS   40
#define I2C_TRACKPAD_IDLE_AFTER     64

#define I2C_TRACKPAD_IRQ            11      // a legacy irq nothing owns, its stub and vector are borrowed
#define I2C_TRACKPAD_MIN_GSI        16      // below this is the legacy lot, never a trackpad
#define I2C_TRACKPAD_WATCHDOG_MS    250     // look anyway after this long, an edge can get lost
#define I2C_TRACKPAD_IRQ_MISSES     4       // reports nothing announced before the interrupt is written off
#define I2C_TRACKPAD_IRQ_STORM      400     // more than this a second isnt a trackpad talking

// for firmware that builds its _CRS at runtime where we cant see it, the addresses
// trackpads use. elan, synaptics/alps, synaptics again, focaltech, cirque, cypress
// theyre probably here but Bleed isnt large enough scale for this to ever be a real problem.
static const uint8_t guess_addrs[] = { 0x15, 0x2C, 0x20, 0x38, 0x2A, 0x24 };

/// @brief Find the trackpad on the I2C buses and get it reporting
/// needs acpi_init() and dw_i2c_init() to have run
void i2c_trackpad_init(void);

// true if i2c_trackpad_init found one, no point starting the task otherwise
bool i2c_trackpad_present(void);

// entry point of the trackpad task. sleeps on the pads interrupt when the firmware
// gives one the ioapic can see, polls it otherwise
void i2c_trackpad_watch(void);

// off the irq stub, the read itself is left to the task
void i2c_trackpad_interrupt(void);
