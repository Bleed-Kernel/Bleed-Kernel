#pragma once

#include <stdint.h>

typedef struct user_mount_info {
    char     path[256];
    char     fs[16];
    char     device[64];
    uint64_t block_size;
    uint64_t blocks_total;
    uint64_t blocks_free;
} user_mount_info_t;
