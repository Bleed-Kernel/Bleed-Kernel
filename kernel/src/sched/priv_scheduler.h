#pragma once

#include <sched/scheduler.h>
#include <mm/spinlock.h>
#define MAX_PIDS    32768

extern spinlock_t sched_lock;

extern task_t *current_task;
extern task_t *task_list_head;

extern task_t *dead_task_head;
extern task_t *dead_task_tail;

int alloc_pid();

void ready_enqueue(task_t *task);
void ready_dequeue(task_t *task);// note to self. these files are getting really, really messy i gotta clean this up
int  sched_has_ready(void);

// these want sched_lock held
task_t *sched_lookup_locked(uint64_t pid);
void    sched_queue_task_locked(task_t *task);
void    sched_unlink_task_locked(task_t *task);

// off every wait queue and the deadline list
void sched_wait_cleanup(task_t *task);
