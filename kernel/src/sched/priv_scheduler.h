#pragma once

#include <sched/scheduler.h>
#define MAX_PIDS    32768

extern uint8_t pid_list[MAX_PIDS];

extern task_t *current_task;
extern task_t *task_queue;
extern task_t *task_list_head;

extern task_t *dead_task_head;
extern task_t *dead_task_tail;

int alloc_pid();
