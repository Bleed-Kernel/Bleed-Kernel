#pragma once

#include <stdint.h>

typedef struct user_ipc_msg {
    uint64_t sender_pid;
    uint64_t addr;
    uint64_t pages;
    uint64_t flags;
} user_ipc_msg_t;

