#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <sched/futex.h>
#include <ACPI/acpi_hpet.h>
#include <user/user_copy.h>
#include <user/errno.h>

typedef struct futex_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
} futex_timespec_t;

// FUTEX_WAIT counts from now, the bitset one is an absolute CLOCK_MONOTONIC time.
// both are in hpet counts
static uint64_t futex_deadline(const futex_timespec_t *ts, int absolute) {
    if (!femtosecondsPerTick)
        return 1;

    uint64_t per_sec = femtosecondsPerSecond / femtosecondsPerTick;
    if ((uint64_t)ts->tv_sec > (UINT64_MAX / 2) / per_sec)
        return 0;   // centuries away, as good as forever

    uint64_t ticks = (uint64_t)ts->tv_sec * per_sec
                   + (uint64_t)ts->tv_nsec * (femtosecondsPerMicrosecond / 1000) / femtosecondsPerTick;
    if (absolute)
        return ticks ? ticks : 1;

    uint64_t now = hpet_read_counter();
    if (ticks >= UINT64_MAX / 2 - now)
        return 0;
    return now + ticks + 1;
}

long sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    int priv = (op & FUTEX_PRIVATE_FLAG) != 0;
    int cmd  = (int)(op & FUTEX_CMD_MASK);

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        uint64_t deadline = 0;
        if (timeout) {
            futex_timespec_t ts;
            if (copy_from_user(task, &ts, (const void *)timeout, sizeof(ts)) != 0)
                return -EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
                return -EINVAL;
            deadline = futex_deadline(&ts, cmd == FUTEX_WAIT_BITSET);
        }
        uint32_t bitset = cmd == FUTEX_WAIT ? FUTEX_BITSET_MATCH_ANY : (uint32_t)val3;
        return futex_wait(task, uaddr, (uint32_t)val, deadline, bitset, priv);
    }

    case FUTEX_WAKE:
        return futex_wake(task, uaddr, (int)val, FUTEX_BITSET_MATCH_ANY, priv);

    case FUTEX_WAKE_BITSET:
        return futex_wake(task, uaddr, (int)val, (uint32_t)val3, priv);

    // the timeout slot carries how many to move
    case FUTEX_REQUEUE:
        return futex_requeue(task, uaddr, uaddr2, (int)val, (int)timeout, 0, 0, priv);

    case FUTEX_CMP_REQUEUE:
        return futex_requeue(task, uaddr, uaddr2, (int)val, (int)timeout, 1, (uint32_t)val3, priv);
    }

    return -ENOSYS;
}
