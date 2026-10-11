#include <sched/scheduler.h>
#include <ACPI/acpi_hpet.h>
#include <mm/spinlock.h>

#include "priv_scheduler.h"

static void event_wake(task_t *task) {
    if (!task->event_wait || task->state != TASK_BLOCKED) return;

    task->state = TASK_READY;
    ready_enqueue(task);
}

static void event_wake_any(task_t *task, void *userdata) {
    (void)userdata;
    event_wake(task);
}

// userdata is the hpet counter
static void event_wake_expired(task_t *task, void *userdata) {
    uint64_t *now = userdata;
    if (!task->event_wait || !task->event_deadline) return;

    if (!*now) *now = hpet_read_counter();
    if (*now >= task->event_deadline)
        event_wake(task);
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
    uint64_t now = 0;
    itterate_each_task(event_wake_expired, &now);
}
