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

#define TTY_IOCTL_GET_FLAGS         0x5401
#define TTY_IOCTL_SET_FLAGS         0x5402

#define TTY_IOCTL_GET_CURSOR        0x5403
#define TTY_IOCTL_SET_CURSOR        0x5404
#define TTY_IOCTL_GET_WINSIZE       0x5405
#define TTY_IOCTL_GET_INDEX         0x5406


#define TTY_IOCTL_TCGETS            0x5407
#define TTY_IOCTL_TCSETS            0x5408
#define TTY_IOCTL_TCSETSW           0x5409
#define TTY_IOCTL_TCSETSF           0x540A

#define TTY_IOCTL_TIOCGWINSZ        0x5413
#define TTY_IOCTL_TIOCSWINSZ        0x5414
#define TTY_IOCTL_FIONBIO           0x5421
#define TTY_IOCTL_CREATE            0x5422
#define TTY_IOCTL_SET_ACTIVE        0x5423

#define TTY_NCCS 7
enum {
    TTY_VINTR = 0,
    TTY_VQUIT,
    TTY_VERASE,
    TTY_VKILL,
    TTY_VEOF,
    TTY_VTIME,
    TTY_VMIN,
};

#define TTY_TERM_ECHO   (1u << 0)
#define TTY_TERM_ICANON (1u << 1)
#define TTY_TERM_ISIG   (1u << 2)

typedef uint32_t tty_tcflag_t;
typedef uint8_t tty_cc_t;

typedef struct {
    tty_tcflag_t c_iflag;
    tty_tcflag_t c_oflag;
    tty_tcflag_t c_cflag;
    tty_tcflag_t c_lflag;
    tty_cc_t c_cc[TTY_NCCS];
} tty_termios_t;

typedef struct tty tty_t;

typedef struct {
    fb_console_t fb;
    uint32_t *display_pixels;
    uint32_t *tty_pixels;
    ansii_state_t ansi;
    spinlock_t fb_lock;
    uint8_t utf8_buf[4];
    int utf8_len;
    int utf8_expected;
    uint8_t is_active;
} tty_fb_backend_t;

struct tty_ops {
    void (*putchar)(tty_t *, char c);
};

typedef struct {
    uint32_t x;
    uint32_t y;
} tty_cursor_t;

typedef struct {
    uint32_t cols;
    uint32_t rows;
} tty_winsize_t;

typedef struct tty{
    INode_t device;
    spinlock_t in_lock;

    char inbuffer[TTY_BUFFER_SZ];
    char outbuffer[TTY_BUFFER_SZ];

    size_t in_head, in_tail;
    size_t out_head, out_tail;
    size_t line_start;

    uint64_t *framebuffer_address;

    uint32_t flags;
    uint32_t index;
    uint8_t activated_once;
    tty_termios_t termios;
    struct tty_ops *ops;    // backend
    void *backend;
} tty_t;

void tty_process_input(tty_t *tty, char c);
void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, fb_console_t *fb, uint32_t flags);
int tty_create_framebuffer(uint32_t *out_index);
int tty_set_active_index(uint32_t index);

void fb_clear(fb_console_t *fb);
void tty_device_init(INode_t* tty_inode);
