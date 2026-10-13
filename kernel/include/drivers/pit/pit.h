#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PIT_CH2_DATA        0x42
#define PIT_COMMAND         0x43
#define PIT_CH2_CONTROL     0x61

#define PIT_CMD_CH2_ONESHOT 0xB0
#define PIT_CH2_GATE        (1 << 0)
#define PIT_CH2_SPEAKER     (1 << 1)
#define PIT_CH2_OUT         (1 << 5)

#define PIT_HZ              1193182
#define PIT_TICKS_PER_MS    (PIT_HZ / 1000)
#define PIT_SPIN_MAX        20000 

bool pit_wait_ms(uint32_t ms);
