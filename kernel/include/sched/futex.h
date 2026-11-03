#pragma once
#include <stdint.h>
#include <sched/scheduler.h>

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10
#define FUTEX_PRIVATE_FLAG      128
#define FUTEX_CLOCK_REALTIME    256     // accepted, but the kernel only has the monotonic clock
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))
#define FUTEX_BITSET_MATCH_ANY  0xFFFFFFFFu

// sleep while *uaddr == val. deadline is in hpet counts
long futex_wait(task_t *task, uint64_t uaddr, uint32_t val, uint64_t deadline, uint32_t bitset, int priv);

// wake up to count sleepers on uaddr, gives back how many it woke
long futex_wake(task_t *task, uint64_t uaddr, int count, uint32_t bitset, int priv);

// wake count on uaddr and move up to requeue of the rest over to uaddr2
long futex_requeue(task_t *task, uint64_t uaddr, uint64_t uaddr2, int count, int requeue,
                   int cmp, uint32_t val3, int priv);
