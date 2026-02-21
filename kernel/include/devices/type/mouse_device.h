#pragma once

#include <fs/vfs.h>
#include <input/mouse_input.h>
#include <mm/spinlock.h>

#define MOUSE_BUFFER_SIZE 128

typedef struct {
    INode_t device;
    spinlock_t lock;
    mouse_event_t buffer[MOUSE_BUFFER_SIZE];
    size_t head;
    size_t tail;
} mouse_device_t;

void mouse_device_init(void);
