#pragma once

#include <sched/scheduler.h>
#include <mm/spinlock.h>
#define MAX_PIDS    32768

extern uint8_t pid_list[MAX_PIDS];
extern spinlock_t sched_lock;

extern task_t *current_task;
extern task_t *task_queue;
extern task_t *task_list_head;

extern task_t *dead_task_head;
extern task_t *dead_task_tail;

extern task_t *ready_head;

int alloc_pid();

void ready_enqueue(task_t *task);
void ready_dequeue(task_t *task);