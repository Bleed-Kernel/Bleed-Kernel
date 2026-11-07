#include <sched/clock.h>
#include <ACPI/acpi_hpet.h>
#include <ACPI/acpi_time.h>
#include <ACPI/acpi.h>
#include <user/errno.h>

#define NS_PER_SEC  1000000000ULL

static int      realtime_read;
static uint64_t realtime_base;  // realtime minus monotonic

// through whole seconds, femtoseconds in 64 bits run out after 5 hours
uint64_t clock_monotonic_ns(void) {
    return clock_counts_to_ns(hpet_read_counter());
}

// the rtc only has whole seconds, its read once and the hpet carries it on
uint64_t clock_realtime_ns(void) {
    if (!realtime_read) {
        struct rtc_time t;
        rtc_get_time(&t);
        int64_t secs = clock_from_civil(t.year, t.mon, t.day, t.hour, t.min, t.sec);
        realtime_base = (uint64_t)secs * NS_PER_SEC - clock_monotonic_ns();
        realtime_read = 1;
    }
    return realtime_base + clock_monotonic_ns();
}

uint64_t clock_realtime_base_ns(void) {
    clock_realtime_ns();
    return realtime_base;
}

int clock_read(int clock, uint64_t *ns) {
    switch (clock) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
            *ns = clock_realtime_ns();
            return 0;
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
        case CLOCK_BOOTTIME:
            *ns = clock_monotonic_ns();
            return 0;
        default:
            return -EINVAL;
    }
}

// split at whole seconds so long times dont overflow on the way
uint64_t clock_ns_to_counts(uint64_t ns) {
    if (!femtosecondsPerTick)
        return 0;
    uint64_t per_sec = femtosecondsPerSecond / femtosecondsPerTick;
    return (ns / NS_PER_SEC) * per_sec + (ns % NS_PER_SEC) * femtosecondsPerNanosecond / femtosecondsPerTick;
}

uint64_t clock_counts_to_ns(uint64_t counts) {
    if (!femtosecondsPerTick)
        return 0;
    uint64_t per_sec = femtosecondsPerSecond / femtosecondsPerTick;
    return (counts / per_sec) * NS_PER_SEC + (counts % per_sec) * femtosecondsPerTick / femtosecondsPerNanosecond;
}
