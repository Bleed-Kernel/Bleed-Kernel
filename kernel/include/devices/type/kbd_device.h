#pragma once

#include <fs/vfs.h>
#include <input/keyboard_input.h>
#include <mm/spinlock.h>

#define KBD_BUFFER_SIZE 128

typedef struct {
    INode_t device;
    spinlock_t lock;
    uint32_t flags;
    uint64_t owner_pid;
    uint64_t owner_pgid;
    keyboard_event_t buffer[KBD_BUFFER_SIZE];
    size_t head;
    size_t tail;
    wait_queue_t wait;
} kbd_device_t;

void kbd_device_init(void);
void kbd_device_wake(void);
