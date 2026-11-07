#pragma once
#include <stdint.h>

#define CLOCK_REALTIME          0
#define CLOCK_MONOTONIC         1
#define CLOCK_MONOTONIC_RAW     4
#define CLOCK_REALTIME_COARSE   5
#define CLOCK_MONOTONIC_COARSE  6
#define CLOCK_BOOTTIME          7

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} clock_timespec_t;

// nanoseconds since boot off the hpet
uint64_t clock_monotonic_ns(void);

// nanoseconds since 1970, one base for every process so they agree
uint64_t clock_realtime_ns(void);
// realtime minus monotonic, read from the rtc the first time
uint64_t clock_realtime_base_ns(void);

// 0 and *ns filled, or -EINVAL for a clock we dont have
int clock_read(int clock, uint64_t *ns);

// hpet counts, what deadlines are measured in
uint64_t clock_ns_to_counts(uint64_t ns);
uint64_t clock_counts_to_ns(uint64_t counts);
