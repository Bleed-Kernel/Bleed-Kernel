#pragma once

#include <stdint.h>
#include <sched/clock.h>

#define EFD_SEMAPHORE   0x1
#define EFD_NONBLOCK    0x800
#define EFD_CLOEXEC     0x80000

#define TFD_TIMER_ABSTIME   0x1
#define TFD_NONBLOCK        0x800
#define TFD_CLOEXEC         0x80000

typedef struct {
    clock_timespec_t interval;
    clock_timespec_t value;
} itimerspec_t;

// a counter in an fd, writes add to it and a read takes it (or one with EFD_SEMAPHORE)
int eventfd_create(uint64_t initval, int flags);

// an fd that reads back how many times its timer went off
int timerfd_create(int clock, int flags);
int timerfd_settime(int fd, int flags, const itimerspec_t *value, itimerspec_t *old);
int timerfd_gettime(int fd, itimerspec_t *cur);
