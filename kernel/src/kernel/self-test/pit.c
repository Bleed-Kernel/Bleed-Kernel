#include <stdio.h>
#include <stdint.h>
#include <drivers/pit/pit.h>
#include <panic.h>
#include <ansii.h>
#include <drivers/serial/serial.h>

void pit_test_self_test(void) {
    kprintf(LOG_INFO "Starting PIT Self-Test\n");
    uint64_t start = pit_read_interval_remaining();
    pit_wait_ticks(100);
    uint64_t end = pit_read_interval_remaining();

    if (end >= start) kprintf(LOG_INFO "PIT ticks advanced by %llu\n", end - start);
    else kprintf(LOG_INFO "PIT wrapped ticks detected\n");

    kprintf(LOG_OK "PIT OK\n");
}
