#include <sched/scheduler.h>
#include <ACPI/acpi_hpet.h>
#include <mm/spinlock.h>

#include "priv_scheduler.h"

extern volatile uint64_t timer_ticks;

static spinlock_t wait_lock;

static unsigned long wait_lock_irq(void) {
    unsigned long flags = irq_push();
    spinlock_acquire(&wait_lock);
    return flags;
}

static void wait_unlock_irq(unsigned long flags) {
    spinlock_release(&wait_lock);
    irq_restore(flags);
}

// reading the hpet on every tick costs more than everything else the tick does, so its only
// looked at on this tick or later. UINT64_MAX means nobody is waiting on a timeout at all
static uint64_t event_check_tick = UINT64_MAX;

// everyone with a deadline, soonest first so the tick only ever looks at the front
static task_t *deadline_head = NULL;

// how many ticks to leave it for a wait with this much hpet time left. a tick is a
// millisecond but they can run slow, half the wait is always early enough to look again
static uint64_t event_ticks_for(uint64_t hpet_left) {
    uint64_t ms = hpet_left / (femtosecondsPerMillisecond / femtosecondsPerTick);
    return ms / 2 ? ms / 2 : 1;
}

static void deadline_insert(task_t *task) {
    task_t *prev = NULL;
    task_t *cur  = deadline_head;
    while (cur && cur->event_deadline <= task->event_deadline) {
        prev = cur;
        cur  = cur->deadline_next;
    }

    task->deadline_prev = prev;
    task->deadline_next = cur;
    if (prev)
        prev->deadline_next = task;
    else
        deadline_head = task;
    if (cur)
        cur->deadline_prev = task;

    task->deadline_queued = 1;
}

static void deadline_remove(task_t *task) {
    if (!task->deadline_queued) return;

    if (task->deadline_prev)
        task->deadline_prev->deadline_next = task->deadline_next;
    else
        deadline_head = task->deadline_next;
    if (task->deadline_next)
        task->deadline_next->deadline_prev = task->deadline_prev;

    task->deadline_prev   = NULL;
    task->deadline_next   = NULL;
    task->deadline_queued = 0;
}

static void wait_entry_add(wait_entry_t *entry, task_t *task) {
    entry->task = task;
    entry->prev = NULL;
    entry->next = entry->queue->head;
    if (entry->next)
        entry->next->prev = entry;
    entry->queue->head = entry;
}

static void wait_entry_remove(wait_entry_t *entry) {
    if (!entry->queue) return;

    if (entry->prev)
        entry->prev->next = entry->next;
    else
        entry->queue->head = entry->next;
    if (entry->next)
        entry->next->prev = entry->prev;

    entry->prev  = NULL;
    entry->next  = NULL;
    entry->queue = NULL;
}

static void wait_cleanup_locked(task_t *task) {
    for (size_t i = 0; i < task->wait_entry_count; i++)
        wait_entry_remove(&task->wait_entries[i]);

    task->wait_entries     = NULL;
    task->wait_entry_count = 0;
    task->event_deadline   = 0;
    deadline_remove(task);
}

void sched_wait_cleanup(task_t *task) {
    unsigned long flags = wait_lock_irq();
    wait_cleanup_locked(task);
    wait_unlock_irq(flags);
}

/// @brief the deadline for a wait of ms starting now, in hpet counter units
uint64_t sched_deadline_ms(uint64_t ms) {
    if (!femtosecondsPerTick) return 1;     // no hpet, already passed so nobody sleeps on it forever
    return hpet_read_counter() + ms * (femtosecondsPerMillisecond / femtosecondsPerTick);
}

int sched_deadline_passed(uint64_t deadline) {
    return hpet_read_counter() >= deadline;
}

void sched_prepare_wait(wait_entry_t *entries, size_t count, uint64_t deadline) {
    task_t *task = cpu_current();
    if (!task) return;

    unsigned long flags = wait_lock_irq();
    for (size_t i = 0; i < count; i++)
        wait_entry_add(&entries[i], task);
    task->wait_entries     = entries;
    task->wait_entry_count = count;

    if (deadline && femtosecondsPerTick) {
        task->event_deadline = deadline;
        deadline_insert(task);

        // make sure the tick looks in time for this one
        uint64_t now  = hpet_read_counter();
        uint64_t tick = timer_ticks + event_ticks_for(deadline > now ? deadline - now : 0);
        if (tick < event_check_tick)
            event_check_tick = tick;
    }

    // a signal could have put it back in line while it was still running
    sched_set_blocked(task);
    wait_unlock_irq(flags);
}

void sched_wait_commit(void) {
    // anything since prepare already made it READY
    if (cpu_current()->state == TASK_BLOCKED)
        asm volatile ("int $" SCHED_YIELD_VECTOR_STR);
}

void sched_finish_wait(void) {
    task_t *task = cpu_current();
    sched_wait_cleanup(task);
    sched_set_running(task);
}

void sched_wait_entries(wait_entry_t *entries, size_t count, uint64_t deadline) {
    if (!cpu_current()) return;

    sched_prepare_wait(entries, count, deadline);
    sched_wait_commit();
    sched_finish_wait();
}

void sched_wait_queue(wait_queue_t *queue, uint64_t deadline) {
    wait_entry_t entry = { .queue = queue };
    sched_wait_entries(&entry, queue ? 1 : 0, deadline);
}

/// @brief block the current task until the deadline or a signal
void sched_sleep_until(uint64_t deadline) {
    if (!deadline) return;
    sched_wait_entries(NULL, 0, deadline);
}

void sched_wake_queue(wait_queue_t *queue) {
    if (!queue) return;

    unsigned long flags = wait_lock_irq();
    for (wait_entry_t *entry = queue->head; entry; entry = entry->next)
        sched_wake_task(entry->task);
    wait_unlock_irq(flags);
}

void sched_wait_queue_destroy(wait_queue_t *queue) {
    if (!queue) return;

    unsigned long flags = wait_lock_irq();
    while (queue->head) {
        wait_entry_t *entry = queue->head;
        sched_wake_task(entry->task);
        wait_entry_remove(entry);
    }
    wait_unlock_irq(flags);
}

// called off the timer, wakes the waits that ran out of time
void sched_event_tick(void) {
    if (timer_ticks < event_check_tick) return;

    unsigned long flags = wait_lock_irq();
    uint64_t now = hpet_read_counter();
    while (deadline_head && now >= deadline_head->event_deadline) {
        task_t *task = deadline_head;
        deadline_remove(task);
        sched_wake_task(task);
    }

    event_check_tick = deadline_head ? timer_ticks + event_ticks_for(deadline_head->event_deadline - now)
                                     : UINT64_MAX;
    wait_unlock_irq(flags);
}
