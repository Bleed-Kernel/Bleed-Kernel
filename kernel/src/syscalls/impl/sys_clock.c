#include <sched/clock.h>
#include <fs/eventfd.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <ACPI/acpi_hpet.h>

long sys_clock_gettime(uint64_t clock, uint64_t user_ts) {
    uint64_t ns;
    int err = clock_read((int)clock, &ns);
    if (err)
        return err;
    clock_timespec_t ts = { (int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL) };
    if (!user_ts || copy_to_user(get_current_task(), (void *)user_ts, &ts, sizeof(ts)) != 0)
        return -EFAULT;
    return 0;
}

long sys_clock_getres(uint64_t clock, uint64_t user_ts) {
    uint64_t ns;
    int err = clock_read((int)clock, &ns);
    if (err)
        return err;
    if (!user_ts)
        return 0;
    // one hpet count, rounded up to a whole nanosecond
    uint64_t res = femtosecondsPerTick ? (femtosecondsPerTick + femtosecondsPerNanosecond - 1) / femtosecondsPerNanosecond : 1;
    clock_timespec_t ts = { 0, (int64_t)res };
    return copy_to_user(get_current_task(), (void *)user_ts, &ts, sizeof(ts)) == 0 ? 0 : -EFAULT;
}

// -ENOSYS while the clock is on the hpet, blibc keeps asking the kernel then
long sys_clockparams(uint64_t user_params) {
    user_clock_params_t p;
    if (!femtosecondsPerTick || !hpet_tsc_params(&p.tsc_base, &p.counts_base, &p.mult))
        return -ENOSYS;
    p.counts_per_sec   = femtosecondsPerSecond / femtosecondsPerTick;
    p.fs_per_tick      = femtosecondsPerTick;
    p.realtime_base_ns = clock_realtime_base_ns();
    if (!user_params || copy_to_user(get_current_task(), (void *)user_params, &p, sizeof(p)) != 0)
        return -EFAULT;
    return 0;
}

long sys_eventfd2(uint64_t initval, uint64_t flags) {
    return eventfd_create(initval & 0xFFFFFFFF, (int)flags);
}

long sys_eventfd(uint64_t initval) {
    return eventfd_create(initval & 0xFFFFFFFF, 0);
}

long sys_timerfd_create(uint64_t clock, uint64_t flags) {
    return timerfd_create((int)clock, (int)flags);
}

long sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t user_new, uint64_t user_old) {
    task_t      *task = get_current_task();
    itimerspec_t val, old;
    if (!user_new || copy_from_user(task, &val, (const void *)user_new, sizeof(val)) != 0)
        return -EFAULT;
    int err = timerfd_settime((int)fd, (int)flags, &val, user_old ? &old : NULL);
    if (err)
        return err;
    if (user_old && copy_to_user(task, (void *)user_old, &old, sizeof(old)) != 0)
        return -EFAULT;
    return 0;
}

long sys_timerfd_gettime(uint64_t fd, uint64_t user_cur) {
    itimerspec_t cur;
    int err = timerfd_gettime((int)fd, &cur);
    if (err)
        return err;
    if (!user_cur || copy_to_user(get_current_task(), (void *)user_cur, &cur, sizeof(cur)) != 0)
        return -EFAULT;
    return 0;
}
