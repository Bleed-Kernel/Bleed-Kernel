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
    size_t size;
    size_t bpp;
} fb_device_t;

struct fb_info {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint64_t bpp;
};

#define FB_IOC_GET_INFO 0x5001
#define FB_IOC_FLIP     0x5002
#define FB_IOC_ACQUIRE  0x5003
#define FB_IOC_RELEASE  0x5004
#define FB_IOC_GET_OWNER 0x5005 // uint64_t pid out, 0 when the tty has it

struct task;

void fb_device_init();

// gives the screen back to the tty if task was the one holding it
void fb_device_task_exit(struct task *task);
