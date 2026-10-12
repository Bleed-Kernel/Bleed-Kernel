#include <sched/scheduler.h>
#include <ACPI/acpi_hpet.h>
#include <mm/spinlock.h>

#include "priv_scheduler.h"

extern volatile uint64_t timer_ticks;

// reading the hpet on every tick costs more than everything else the tick does, so its only
// looked at on this tick or later. UINT64_MAX means nobody is waiting on a timeout at all
static uint64_t event_check_tick = UINT64_MAX;

// how many ticks to leave it for a wait with this much hpet time left. a tick is a
// millisecond but they can run slow, half the wait is always early enough to look again
static uint64_t event_ticks_for(uint64_t hpet_left) {
    uint64_t ms = hpet_left / (femtosecondsPerMillisecond / femtosecondsPerTick);
    return ms / 2 ? ms / 2 : 1;
}

static void event_wake(task_t *task) {
    if (!task->event_wait || task->state != TASK_BLOCKED) return;

    task->state = TASK_READY;
    ready_enqueue(task);
}

static void event_wake_any(task_t *task, void *userdata) {
    (void)userdata;
    event_wake(task);
}

typedef struct {
    uint64_t now;       // the hpet counter
    uint64_t soonest;   // least time left of the waits that havent run out
} event_expiry_t;

static void event_wake_expired(task_t *task, void *userdata) {
    event_expiry_t *e = userdata;
    if (!task->event_wait || !task->event_deadline) return;

    if (e->now >= task->event_deadline) {
        event_wake(task);
        return;
    }
    if (task->event_deadline - e->now < e->soonest)
        e->soonest = task->event_deadline - e->now;
}

/// @brief the deadline for a wait of ms starting now, in hpet counter units
uint64_t sched_deadline_ms(uint64_t ms) {
    if (!femtosecondsPerTick) return 1;     // no hpet, already passed so nobody sleeps on it forever
    return hpet_read_counter() + ms * (femtosecondsPerMillisecond / femtosecondsPerTick);
}

int sched_deadline_passed(uint64_t deadline) {
    return hpet_read_counter() >= deadline;
}

/// @brief block the current task until an event, a signal or the deadline
/// @param deadline from sched_deadline_ms, 0 waits forever
void sched_wait_event(uint64_t deadline) {
    task_t *task = current_task;
    if (!task) return;

    // make sure the tick looks in time for this one
    if (deadline && femtosecondsPerTick) {
        uint64_t now  = hpet_read_counter();
        uint64_t tick = timer_ticks + event_ticks_for(deadline > now ? deadline - now : 0);
        if (tick < event_check_tick)
            event_check_tick = tick;
    }

    task->event_deadline = deadline;
    task->event_wait     = 1;
    task->state          = TASK_BLOCKED;
    asm volatile ("int $" SCHED_YIELD_VECTOR_STR);

    task->event_wait     = 0;
    task->event_deadline = 0;
}

/// @brief something happened, let every task in sched_wait_event look again
void sched_wake_event_waiters(void) {
    itterate_each_task(event_wake_any, NULL);
}

// called off the timer, wakes the waits that ran out of time
void sched_event_tick(void) {
    if (timer_ticks < event_check_tick) return;

    event_expiry_t e = { .now = hpet_read_counter(), .soonest = UINT64_MAX };
    itterate_each_task(event_wake_expired, &e);

    event_check_tick = e.soonest == UINT64_MAX ? UINT64_MAX : timer_ticks + event_ticks_for(e.soonest);
}
