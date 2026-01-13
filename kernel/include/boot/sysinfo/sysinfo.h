#pragma once

#include <stdint.h>

typedef struct system_memory_info{
    uint64_t MEMORY_USABLE;
    uint64_t MEMORY_RESERVED;
    uint64_t MEMORY_FAULTY;
} system_memory_info_t;

system_memory_info_t *get_system_memory_info();