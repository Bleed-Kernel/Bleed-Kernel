#pragma once

#include <fs/vfs.h>
#include <mm/spinlock.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    INode_t device;
    spinlock_t lock;
    uintptr_t fb_phys;
    size_t width;
    size_t height;
    size_t pitch;
} fb_device_t;

__attribute__((used))
static fb_device_t *fb0 = NULL;

void fb_device_init(void);