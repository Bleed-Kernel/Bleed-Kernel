#pragma once
#include <stdint.h>
#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <drivers/framebuffer/framebuffer.h>
#include <mm/spinlock.h>
#include <stddef.h>

#define TTY_BUFFER_SZ   1024
#define TTY_ECHO        (1 << 1)
#define TTY_CANNONICAL  (1 << 2)
#define TTY_NONBLOCK    (1 << 4)

#define TTY_IOCTL_GET_FLAGS     0x5401
#define TTY_IOCTL_SET_FLAGS     0x5402

#define TTY_IOCTL_GET_CURSOR    0x5403
#define TTY_IOCTL_SET_CURSOR    0x5404

typedef struct tty tty_t;

typedef struct {
    fb_console_t fb;
    ansii_state_t ansi;
    spinlock_t fb_lock;
    uint8_t utf8_buf[4];
    int utf8_len;
    int utf8_expected;
} tty_fb_backend_t;

struct tty_ops {
    void (*putchar)(tty_t *, char c);
};

typedef struct {
    uint32_t x;
    uint32_t y;
} tty_cursor_t;

typedef struct tty{
    INode_t device;

    char inbuffer[TTY_BUFFER_SZ];
    char outbuffer[TTY_BUFFER_SZ];

    size_t in_head, in_tail;
    size_t out_head, out_tail;
    size_t line_start;

    uint64_t *framebuffer_address;

    uint32_t flags;
    struct tty_ops *ops;    // backend
    void *backend;
} tty_t;

void tty_process_input(tty_t *tty, char c);
void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, fb_console_t *fb, uint32_t flags);

void fb_clear(fb_console_t *fb);
void tty_device_init(INode_t* tty_inode);