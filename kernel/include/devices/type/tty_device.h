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
#define TTY_IOCTL_GET_ACTIVE_INDEX  0x5424

#define TTY_IOCTL_SCROLL            0x5425

// offscreen ttys, the same terminal as tty0 but drawing into memory for a window to show
#define TTY_IOCTL_DESTROY           0x5430
#define TTY_IOCTL_GRAB              0x5431
#define TTY_IOCTL_RESIZE            0x5432  // tty_create_t again, new size in and the index left alone
#define TTY_IOCTL_GRAB_TEXT         0x5433  // tty_text_grab_t, the same as GRAB but the text instead of pixels

#define TTY_OFFSCREEN_MIN           64      // pixels, either way
#define TTY_OFFSCREEN_MAX           4096
#define TTY_OFFSCREEN_GROW          128     // buffers are sized up to a multiple of this

#define SCROLLBACK_ROWS 500

typedef struct {
    char c;
    uint32_t fg;
    uint32_t bg;
} tty_char_t;

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

// TTY_IOCTL_CREATE, size in pixels going in and which /dev/ttyN it became coming out
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t index;
} tty_create_t;

// TTY_IOCTL_GRAB, buffer is a whole width by height of pixels. only the rows that changed
// since the last grab get written, top to bottom says which they were (both 0 for none).
// pitch is how many pixels apart the rows of buffer start, 0 if thats just the ttys width
typedef struct {
    uint64_t buffer;
    uint32_t top;
    uint32_t bottom;
    uint32_t pitch;
} tty_grab_t;

// TTY_IOCTL_GRAB_TEXT, for a window that draws the text itself. cells is room for max_rows rows of
// fb_cell_t, stride apart, laid out like the screen. only the rows that changed since the last grab
// (of either kind) get written, top to bottom says which. empty cells are cp 0 and should be bg.
// a size given to CREATE or RESIZE is cell_w by cell_h pixels for every column and row
typedef struct {
    uint64_t cells;
    uint32_t stride;
    uint32_t max_rows;
    uint32_t cols;
    uint32_t rows;
    uint32_t top;
    uint32_t bottom;
    uint32_t bg;
    uint32_t cell_w;
    uint32_t cell_h;
} tty_text_grab_t;

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
    uint32_t cap_height;    // rows the buffers of an offscreen tty really have, fb.pitch is how wide
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
    uint8_t offscreen;      // draws into its own buffer instead of the screen
    uint64_t owner_pid;     // who made it with TTY_IOCTL_CREATE, 0 for tty0 or a free slot
    tty_termios_t termios;
    struct tty_ops *ops;    // backend
    void *backend;
    struct tty *next;       // the offscreen ones are a list, oldest first
    wait_queue_t wait;
} tty_t;

void tty_process_input(tty_t *tty, char c);
void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, fb_console_t *fb, uint32_t flags);
int tty_create_framebuffer(uint32_t *out_index);
int tty_set_active_index(uint32_t index);
void tty_set_graphics(int on);

struct task;

// frees any offscreen ttys task made
void tty_task_exit(struct task *task);

void fb_clear(fb_console_t *fb);
void tty_device_init(void);
