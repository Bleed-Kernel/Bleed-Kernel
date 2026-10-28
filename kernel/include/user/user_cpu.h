#pragma once
#include <stdint.h>

typedef struct user_cpu_info {
    uint32_t        id;
    uint32_t        lapic_id;
    uint64_t        busy_time;
    uint64_t        idle_time;
    uint64_t        current_pid;
} user_cpu_info_t;
