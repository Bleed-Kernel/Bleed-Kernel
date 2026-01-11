#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <devices/device_io.h>
#include <mm/spinlock.h>
#include <stdio.h>

long tty_read(device_t *dev, void *buf, size_t len) {
    tty_t *tty = dev->priv;

    const int mode_canonical = tty->flags & TTY_CANNONICAL;
    //const int mode_echo = tty->flags & TTY_ECHO;  might need it one day

    if (tty->in_head == tty->in_tail)
        return 0;

    if (mode_canonical) {
        size_t i = tty->in_tail;
        int found = 0;

        while (i != tty->in_head) {
            if (tty->inbuffer[i] == '\n') {
                found = 1;
                break;
            }
            i = (i + 1) % TTY_BUFFER_SZ;
        }

        if (!found)
            return 0;
    }

    size_t count = 0;
    while (count < len && tty->in_tail != tty->in_head) {
        char c = tty->inbuffer[tty->in_tail++ % TTY_BUFFER_SZ];
        ((char *)buf)[count++] = c;
        if ((mode_canonical) && c == '\n')
            break;
    }

    return count;
}

int tty_write(device_t *dev, const void *buf, size_t len) {
    tty_t *tty = dev->priv;
    const char *c = buf;

    void (*putc)(tty_t*, char) = tty->ops->putchar;
    for (size_t i = 0; i < len; i++){
        putc(tty, c[i]);
    }

    return len;
}

int tty_ioctl(device_t *dev, unsigned long req, void *arg){
    tty_t *tty = dev->priv;

    switch (req) {
    case 0:
        *(uint32_t *)arg = tty->flags;
        return 0;

    case 1:
        tty->flags = *(uint32_t *)arg;
        return 0;
    }

    return -1;
}

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