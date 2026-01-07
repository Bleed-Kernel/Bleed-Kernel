#define NANOPRINTF_IMPLEMENTATION
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS     0
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS       0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS           1
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS           1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS          0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS       0
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS           0

#include <drivers/framebuffer/framebuffer.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <lib/nanoprintf.h>
#include <mm/kalloc.h>
#include <drivers/serial/serial.h>
#include <console/console.h>
#include <devices/type/tty_device.h>
#include <mm/spinlock.h>

/// @brief formatted print to tty
/// @param s string
void kprintf(const char *fmt, ...) {
    char *buf = NULL;
    size_t size = 256;

    while (1) {
        buf = kmalloc(size);
        if (!buf) return;

        va_list args;
        va_start(args, fmt);
        int written = npf_vsnprintf(buf, size, fmt, args);
        va_end(args);

        if (written < 0) {
            kfree(buf, size);
            return;
        }

        if ((size_t)written < size) {
            break;
        }

        kfree(buf, size);
        size *= 2;
    }

    tty_t *tty = NULL;
    device_t *dev = device_get_by_name("tty0");
    if (dev)
        tty = dev->priv;

    if (tty && tty->backend) {
        fb_console_t *fb = &((tty_fb_backend_t *)tty->backend)->fb;
        ansii_state_t *ansi = &((tty_fb_backend_t *)tty->backend)->ansi;
        spinlock_t framebuffer_lock = ((tty_fb_backend_t *)tty->backend)->fb_lock;
        framebuffer_write_string(fb, ansi, buf, &framebuffer_lock);
    }

    serial_printf("%s", buf);
}
