#pragma once
#include <stdint.h>
#include <sched/scheduler.h>

typedef struct user_task_info {
    uint64_t        id;
    // Process relationship identifiers exported to userspace tools.
    uint64_t        ppid;
    uint64_t        pgid;
    uint64_t        sid;
    task_state_t    state;
    task_privil_t   privilege_level;
    uint32_t        quantum_remaining;
    char            name[128];
} user_task_info_t;

typedef struct user_task_usage {
    uint64_t        cpu_time;       // tsc cycles on the cpu so far, only good for comparing
    uint64_t        mem_pages;      // pages of ram mapped into the task
} user_task_usage_t;
