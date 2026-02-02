#pragma once
#include <stddef.h>
#include <sched/scheduler.h>

int copy_to_user(task_t *user_task, void *user_dst, const void *kernel_src, size_t len);
int copy_from_user(task_t *user_task, void *kernel_dst, const void *user_src, size_t len);
