#pragma once
#include <sched/scheduler.h>
#include <stdint.h>

// window of user address space that mmap, the framebuffer mapping and ipc hand out
#define USER_MMAP_BASE  0x0000004000000000ULL
#define USER_MMAP_LIMIT 0x00007fffffe00000ULL

typedef struct user_heap {
    uintptr_t current;
    uintptr_t end;
    struct task* task;
} user_heap_t;