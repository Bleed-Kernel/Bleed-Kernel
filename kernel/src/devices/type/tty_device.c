#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <devices/device_io.h>
#include <mm/spinlock.h>
#include <stdio.h>

static void tty_fb_putchar(tty_t *tty, char c) {
    tty_fb_backend_t *b = tty->backend;
    framebuffer_ansi_char(&b->fb, &b->fb_lock, &b->ansi, c);
}

static void tty_fb_clear(tty_t *tty) {
    uint32_t* buffer = framebuffer_get_buffer();
    fb_console_t *fb = &((tty_fb_backend_t *)tty->backend)->fb;

    for (uint64_t y = 0; y < fb->height; y++)
        for (uint64_t x = 0; x < fb->width; x++)
            buffer[y * fb->pitch + x] = fb->bg;

    fb->cursor_x = 0;
    fb->cursor_y = 0;

    framebuffer_blit(buffer, fb->pixels, fb->width, fb->height);
}

static struct tty_ops fb_ops = {
    .putchar = tty_fb_putchar,
    .clear = tty_fb_clear,
};

void tty_process_input(tty_t *tty, char c) {
    if (c == '\b') {
        if (tty->in_head != tty->in_tail) {
            tty->in_head--;
                if (tty->flags & TTY_ECHO) {
                    tty->ops->putchar(tty, '\b');
                    tty->ops->putchar(tty, ' ');
                    tty->ops->putchar(tty, '\b');
                }
        }
        return;
    }

    size_t head = tty->in_head;
    tty->inbuffer[head] = c;
    tty->in_head = (head + 1) % TTY_BUFFER_SZ;

    if (tty->flags & TTY_ECHO)
        tty->ops->putchar(tty, c);
}

void tty_init(tty_t *tty, const char *name,
              struct tty_ops *ops, void *backend,
              spinlock_t lock, uint32_t flags) {
    memset(tty, 0, sizeof(*tty));

    tty->flags   = flags;
    tty->ops     = ops;
    tty->backend = backend;

    tty->device.name  = name;
    tty->device.read  = tty_read;
    tty->device.write = tty_write;
    tty->device.ioctl = tty_ioctl;
    tty->device.priv  = tty;
    
    spinlock_init(&lock);
}

void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, const char *name, fb_console_t *fb, uint32_t flags) {
    spinlock_t framebuffer_lock = {0};

    backend->fb   = *fb;
    backend->ansi = (ansii_state_t){0};
    backend->fb_lock = framebuffer_lock;

    tty_init(tty, name, &fb_ops, backend, backend->fb_lock, flags);
}