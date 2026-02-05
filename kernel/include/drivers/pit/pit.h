#pragma once
#include <stdint.h>
#include <cpu/io.h>

#define PIT_FREQUENCY   1193182

extern volatile uint64_t timer_ticks;

/// @brief Sets the reload divisor for PIT-0
/// @param count PIT Clock Ticks per Period
void pit_set_event_counter(uint32_t count);


/// @brief Current countdown value of PIT-0
/// @return PIT fires until countdown is 0
uint16_t pit_read_interval_remaining();

void pit_wait_ticks(uint32_t ticks);

void pit_init(uint32_t frequency);

void pit_wait_seconds(double seconds);

void pit_countdown_timer(double seconds);