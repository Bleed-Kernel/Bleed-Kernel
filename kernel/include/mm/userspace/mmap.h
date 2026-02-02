#pragma once
#include <sched/scheduler.h>
#include <stdint.h>

typedef struct user_heap {
    uintptr_t current;
    uintptr_t end;
    struct task* task;
} user_heap_t;