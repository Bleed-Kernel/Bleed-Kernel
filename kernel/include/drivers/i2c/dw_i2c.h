#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
    the controller behind Intel LPSS and the AMD FCH, ive tried to make the comments as clear as
    possible but if i wanted to write a novelle about I2C I would've chosen a different hobby.
    https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-designware-core.h
*/

#define DW_I2C_MAX_BUSES        8

// register offsets
#define DW_IC_CON               0x00
#define DW_IC_TAR               0x04
#define DW_IC_DATA_CMD          0x10
#define DW_IC_FS_SCL_HCNT       0x1C
#define DW_IC_FS_SCL_LCNT       0x20
#define DW_IC_INTR_MASK         0x30
#define DW_IC_RAW_INTR_STAT     0x34
#define DW_IC_CLR_INTR          0x40    // reading it clears them all
#define DW_IC_CLR_TX_ABRT       0x54
#define DW_IC_ENABLE            0x6C
#define DW_IC_STATUS            0x70
#define DW_IC_SDA_HOLD          0x7C
#define DW_IC_ENABLE_STATUS     0x9C
#define DW_IC_COMP_PARAM_1      0xF4
#define DW_IC_COMP_TYPE         0xFC

// IC_CON
#define DW_CON_MASTER           (1u << 0)
#define DW_CON_SPEED_FAST       (2u << 1)   // 400kHz
#define DW_CON_RESTART_EN       (1u << 5)
#define DW_CON_SLAVE_DISABLE    (1u << 6)

// IC_DATA_CMD, the low byte is the data
#define DW_CMD_READ             (1u << 8)
#define DW_CMD_STOP             (1u << 9)   // stop after this byte
#define DW_CMD_RESTART          (1u << 10)  // repeated start before it

// IC_RAW_INTR_STAT
#define DW_INTR_TX_ABRT         (1u << 6)   // nobody acked or we lost the bus
#define DW_INTR_STOP_DET        (1u << 9)

// IC_STATUS
#define DW_STATUS_TFNF          (1u << 1)   // tx fifo not full
#define DW_STATUS_RFNE          (1u << 3)   // rx fifo not empty
#define DW_STATUS_MST_ACTIVITY  (1u << 5)

#define DW_ENABLE_EN            (1u << 0)

#define DW_COMP_TYPE_VALUE      0x44570140  // "DW" + the part number, every one of them reads this
#define DW_RX_DEPTH_FALLBACK    8           // for when COMP_PARAM_1 isnt filled in

// 400kHz timings in input clock cycles, the fall time is counted in like linux does
#define DW_FS_SCL_HCNT(mhz)     ((mhz) * 9 / 10)    // 0.6us high + 0.3us fall
#define DW_FS_SCL_LCNT(mhz)     ((mhz) * 16 / 10)   // 1.3us low + 0.3us fall
#define DW_SDA_HOLD(mhz)        ((mhz) * 23 / 100)  // 230ns

#define DW_CLOCK_AMD_MHZ        150
#define DW_CLOCK_UNKNOWN_MHZ    216

// LPSS private registers, they sit after the DesignWare ones in the same BAR
#define LPSS_PRIV_RESETS        0x204

#define LPSS_RESETS_FUNC        (3u << 0)
#define LPSS_RESETS_IDMA        (1u << 2)

#define PCI_DEVICE_PCH_LPC      0x1F        // device 31 is the LPC/SMBus/SPI flash lot, never LPSS

#define DW_ENABLE_TIMEOUT_MS    25
#define DW_XFER_TIMEOUT_MS      50          // how long the bus can sit still before we call it stuck

#define DW_SLEEP_BYTES          24          // a byte is 22us on the wire, fewer than this to come and its quicker to spin

#define DW_TARGET_NONE          0xFFFF

typedef struct dw_i2c {
    bool     present;
    uint64_t base;      // vaddr of the registers
    uint16_t target;    // address its set up for, DW_TARGET_NONE to force setting it up again
    uint16_t rx_depth;
} dw_i2c_t;

// one run of consecutive pci device ids and the clock LPSS feeds them
typedef struct lpss_i2c_id {
    uint16_t first;
    uint8_t  count;
    uint16_t clock_mhz;
} lpss_i2c_id_t;

// Find every DesignWare I2C controller and set it up as a 400kHz master
void dw_i2c_init(void);

// nth controller that came up, NULL once you run out
dw_i2c_t *dw_i2c_get_bus(int index);

// Write out_len bytes then read in_len bytes back in the one transaction, either can be 0
// returns the number of bytes read, -1 if the device nacked, -2 if the bus got stuck
int dw_i2c_xfer(dw_i2c_t *bus, uint8_t addr, const void *out, size_t out_len,
                void *in, size_t in_len);

// Read where the first two bytes are the length of the whole thing, them included
// a length that cant be right reads 3 bytes and stops, max must be at least 3
int dw_i2c_read_prefixed(dw_i2c_t *bus, uint8_t addr, void *in, size_t max);
