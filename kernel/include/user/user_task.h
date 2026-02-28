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
