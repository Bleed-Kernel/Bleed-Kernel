#pragma once
#include <stddef.h>
#include <sched/scheduler.h>

int user_ptr_valid(uint64_t ptr);

int copy_to_user(task_t *user_task, void *user_dst, const void *kernel_src, size_t len);
int copy_user_string(task_t *caller, const char *user_src, char *kernel_dst, size_t dst_len);
long user_strnlen(task_t *caller, const char *user_src, size_t max_len);
long copy_user_path(task_t *caller, const char *user_src, char *kernel_dst, size_t dst_len);

int copy_from_user(task_t *user_task, void *kernel_dst, const void *user_src, size_t len);

// hhdm pointer to a user byte so the kernel can work on it in place, NULL if it cant
void *user_page_ptr(task_t *task, uintptr_t addr, int write);
