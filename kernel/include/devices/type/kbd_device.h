#pragma once

#define KBD_BUFFER_SIZE 128

typedef struct {
    INode_t device;
    spinlock_t lock;
    keyboard_event_t buffer[KBD_BUFFER_SIZE];
    size_t head;
    size_t tail;
} kbd_device_t;

void kbd_device_init(void);
