#pragma once
#include <stdint.h>
#include <stddef.h>

struct task;
struct wait_queue;

// one per queue a task sleeps on, they live on the sleepers stack
typedef struct wait_entry {
    struct task         *task;
    struct wait_queue   *queue;     // NULL once the queue went away under it
    struct wait_entry   *prev;
    struct wait_entry   *next;
} wait_entry_t;

typedef struct wait_queue {
    wait_entry_t *head;
} wait_queue_t;

// sleep on every entries queue till one is woken, a signal or the deadline. interrupts must be off
void sched_wait_entries(wait_entry_t *entries, size_t count, uint64_t deadline);

// sched_wait_entries with the one queue, NULL is a plain sleep
void sched_wait_queue(wait_queue_t *queue, uint64_t deadline);

// wake everyone sleeping on queue
void sched_wake_queue(wait_queue_t *queue);

// wake everyone and let go of them, for a queue thats about to be freed
void sched_wait_queue_destroy(wait_queue_t *queue);
