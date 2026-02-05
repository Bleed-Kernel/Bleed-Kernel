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

struct fb_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint64_t bpp;
};

#define FB_IOC_GET_INFO 0x5001
#define FB_IOC_FLIP     0x5002

void fb_device_init();