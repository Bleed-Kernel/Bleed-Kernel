#define NANOPRINTF_IMPLEMENTATION
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS     1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS       1
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS           1
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS           1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS          1
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS       1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS           0

#include <drivers/framebuffer/framebuffer.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <lib/nanoprintf.h>
#include <drivers/serial/serial.h>
#include <console/console.h>
#include <devices/type/tty_device.h>
#include <mm/spinlock.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    return npf_vsnprintf(buf, size, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

#define KPRINTF_CHUNK 256

typedef struct {
    char             buf[KPRINTF_CHUNK];
    size_t           len;
    tty_fb_backend_t *backend;
} kprintf_ctx_t;

static void kprintf_flush(kprintf_ctx_t *k) {
    if (!k->len)
        return;
    k->buf[k->len] = '\0';
    if (k->backend)
        framebuffer_write_string(&k->backend->fb, &k->backend->ansi, k->buf, &k->backend->fb_lock);
    serial_printf("%s", k->buf);
    k->len = 0;
}

static void kprintf_putc(int c, void *ctx) {
    kprintf_ctx_t *k = ctx;
    k->buf[k->len++] = (char)c;
    if (k->len == KPRINTF_CHUNK - 1)
        kprintf_flush(k);
}

/// @brief formatted print to tty
/// @param s string
void kprintf(const char *fmt, ...) {
    kprintf_ctx_t k = { .len = 0, .backend = NULL };

    // Output to tty framebuffer if available
    INode_t *dev = device_get_by_name("tty0");
    if (dev) {
        tty_t *tty = dev->internal_data;
        if (tty && tty->backend)
            k.backend = (tty_fb_backend_t *)tty->backend;
    }

    va_list args;
    va_start(args, fmt);
    npf_vpprintf(kprintf_putc, &k, fmt, args);
    va_end(args);
    kprintf_flush(&k);
}

void kprintf_pos(uint64_t x, uint64_t y, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int written = npf_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written < 0)
        return;
    buf[sizeof(buf) - 1] = '\0';

    INode_t *dev = device_get_by_name("tty0");
    if (!dev) return;

    tty_t *tty = dev->internal_data;
    if (!tty || !tty->backend) return;

    tty_fb_backend_t *b = tty->backend;
    fb_console_t *fb = &b->fb;
    ansii_state_t *ansi = &b->ansi;
    spinlock_t *lock = &b->fb_lock;

    uint64_t cols = fb->width / fb->font->width;
    if (x >= cols)
        return;
        
    /* save cursor */
    uint64_t old_x = fb->cursor_x;
    uint64_t old_y = fb->cursor_y;

    fb->cursor_x = x;
    fb->cursor_y = y;

    size_t len = strlen(buf);
    size_t max = cols - fb->cursor_x;
    if (len > max)
        buf[max] = '\0';

    framebuffer_write_string(fb, ansi, buf, lock);

    fb->cursor_x = old_x;
    fb->cursor_y = old_y;
}

