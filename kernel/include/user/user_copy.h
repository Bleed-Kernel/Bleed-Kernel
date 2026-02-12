#pragma once
#include <stddef.h>
#include <sched/scheduler.h>

int user_ptr_valid(uint64_t ptr);

int copy_to_user(task_t *user_task, void *user_dst, const void *kernel_src, size_t len);
int copy_user_string(task_t *caller, const char *user_src, char *kernel_dst, size_t dst_len);

int copy_from_user(task_t *user_task, void *kernel_dst, const void *user_src, size_t len);
