#pragma once

#include <fs/vfs.h>
#include <input/touch_input.h>
#include <mm/spinlock.h>

#define TOUCH_BUFFER_SIZE 128

typedef struct {
    INode_t device;
    spinlock_t lock;
    touch_event_t buffer[TOUCH_BUFFER_SIZE];
    size_t head;
    size_t tail;
    wait_queue_t wait;
} touch_device_t;

void touch_device_init(void);
