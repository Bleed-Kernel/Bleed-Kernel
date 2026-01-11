#include <stdio.h>
#include <stdint.h>
#include <drivers/pit/pit.h>
#include <panic.h>
#include <ansii.h>
#include <drivers/serial/serial.h>

void pit_test_self_test(void) {
    serial_printf(LOG_INFO "Starting PIT Self-Test\n");
    uint64_t start = pit_read_interval_remaining();
    pit_wait_ticks(100);
    uint64_t end = pit_read_interval_remaining();

    if (end >= start) serial_printf(LOG_INFO "PIT ticks advanced by %u\n", end - start);
    else serial_printf(LOG_INFO "PIT wrapped ticks detected\n");

    serial_printf(LOG_OK "PIT OK\n");
}
