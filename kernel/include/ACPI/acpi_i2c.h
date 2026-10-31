#pragma once
#include <ACPI/acpi.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ACPI_I2C_MAX_DEVICES    16
#define ACPI_I2C_HID_WINDOW     1024    // how far back from a _CRS we look for the _HID it belongs to
#define ACPI_I2C_DEVICE_WINDOW  16384   // how far back we look for the Device() a _CRS sits in

// I2cSerialBus resource desc
#define ACPI_RES_SERIAL_BUS     0x8E
#define ACPI_RES_I2C_REVISION   3
#define ACPI_RES_I2C_BUS_TYPE   5
#define ACPI_RES_I2C_TYPE_REV   9
#define ACPI_RES_I2C_TYPE_LEN   10
#define ACPI_RES_I2C_SPEED      12
#define ACPI_RES_I2C_ADDRESS    16
#define ACPI_RES_I2C_MIN_LENGTH 18
#define ACPI_SERIAL_BUS_I2C     0x01

// the other descriptors a _CRS can have after it
#define ACPI_RES_WALK_MAX       16
#define ACPI_RES_LARGE          0x80
#define ACPI_RES_END_TAG        0x78    // small ones keep their length in the low 3 bits
#define ACPI_RES_EXT_IRQ        0x89
#define ACPI_RES_IRQ_FLAGS      3
#define ACPI_RES_IRQ_COUNT      4
#define ACPI_RES_IRQ_FIRST      5
#define ACPI_RES_IRQ_EDGE       (1 << 1)
#define ACPI_RES_IRQ_LOW        (1 << 2)
#define ACPI_RES_GPIO           0x8C
#define ACPI_RES_GPIO_TYPE      4       // 0 = GpioInt
#define ACPI_RES_GPIO_PINS      14      // offset of the pin table

#define AML_NAME_OP             0x08
#define AML_DWORD_PREFIX        0x0C
#define AML_STRING_PREFIX       0x0D
#define AML_EXT_OP              0x5B
#define AML_DEVICE_OP           0x82

#define ACPI_HID_OVER_I2C       "PNP0C50"

typedef struct acpi_i2c_dev {
    char     hid[9];        // _HID of the device
    uint16_t addr;
    bool     hid_over_i2c;

    uint32_t irq;           // gsi of its Interrupt(), 0 if it didnt come with one
    bool     irq_level;
    bool     irq_low;
    bool     gpio_irq;      // interrupts through a gpio controller instead
    uint16_t gpio_pin;
} acpi_i2c_dev_t;

/// @brief List the I2C devices the firmware describes in the DSDT and SSDTs
/// theres no aml interpreter so this only sees the ones written out as plain resource templates
/// @param out filled in
/// @param max room in out
/// @return how many were found
size_t acpi_i2c_devices(acpi_i2c_dev_t *out, size_t max);
