#pragma once

#include <stdint.h>
#include <sched/scheduler.h>

typedef struct ipc_message {
    uint64_t sender_pid;
    uint64_t addr;
    uint64_t pages;
    uint64_t flags;
    struct ipc_message *next;
} ipc_message_t;

long ipc_send_pages(task_t *sender, uint64_t target_pid, uint64_t src_addr, uint64_t pages);
long ipc_recv(task_t *receiver, uint64_t user_msg_ptr);
void ipc_task_cleanup(task_t *task);

