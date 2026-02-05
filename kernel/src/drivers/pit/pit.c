#include <stdint.h>
#include <cpu/io.h>
#include <drivers/pit/pit.h>
#include <panic.h>
#include <stdio.h>

static uint16_t pit_reload = 0;
static uint16_t pit_last_count = 0;

void pit_wait_ticks(uint32_t ticks) {
    if (ticks == 0) return;

    uint16_t last = pit_read_interval_remaining();

    while (ticks > 0) {
        uint16_t current = pit_read_interval_remaining();
        uint16_t delta;

        if (last >= current)
            delta = last - current;
        else
            delta = last + (pit_reload - current);

        if (delta == 0) continue;

        if (delta >= ticks) break;
        ticks -= delta;
        last = current;
    }
}

void pit_init(uint32_t frequency) {
    if (frequency == 0 || frequency > PIT_FREQUENCY) {
        ke_panic("Invalid PIT frequency");
    }

    uint32_t divisor = PIT_FREQUENCY / frequency;
    pit_reload = (uint16_t)divisor;

    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);

    pit_last_count = pit_reload;
}

uint16_t pit_read_interval_remaining() {
    outb(0x43, 0x00);
    uint16_t count = inb(0x40);
    count |= inb(0x40) << 8;
    return count;
}

double pit_delta() {
    uint16_t current = pit_read_interval_remaining();
    uint16_t delta;

    if (pit_last_count >= current) {
        delta = pit_last_count - current;
    } else {
        delta = pit_last_count + (pit_reload - current);
    }

    pit_last_count = current;
    return (double)delta / PIT_FREQUENCY;
}

void pit_wait_seconds(double seconds) {
    pit_last_count = pit_read_interval_remaining();
    double elapsed = 0.0;

    while (elapsed < seconds) {
        elapsed += pit_delta();
    }
}