#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <input/keyboard_input.h>
#include <input/keyboard_dispatch.h>
#include <devices/device_io.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <console/console.h>
#include <stdio.h>

long tty_read(INode_t *dev, void *buf, size_t len, size_t offset) {
    (void)offset;
    tty_t *tty = dev->internal_data;

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

long tty_inode_write(INode_t* inode, const void* in_buffer, size_t size, size_t offset) {
    (void) offset;
    tty_t *tty = inode->internal_data;
    const char *c = in_buffer;

    void (*putc)(tty_t*, char) = tty->ops->putchar;
    for (size_t i = 0; i < size; i++){
        putc(tty, c[i]);
    }

    return size;
}

int tty_ioctl(INode_t *dev, unsigned long req, void *arg){
    tty_t *tty = dev->internal_data;

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

static void tty_input_listener(const keyboard_event_t *ev) {
    if (ev->action != KEY_DOWN)
        return;

    char c = tty_key_to_ascii(ev);
    if (!c)
        return;

    tty_t *tty =
        (tty_t*)console_get_active_console()->internal_data;
    tty_process_input(tty, c);
}

static void tty_fb_putchar(tty_t *tty, char c) {
    tty_fb_backend_t *b = tty->backend;
    framebuffer_ansi_char(&b->fb, &b->fb_lock, &b->ansi, c);
}

struct tty_ops tty_ops = {
    .putchar = tty_fb_putchar,
};

struct INodeOps tty_inode_ops = {
    .write = tty_inode_write,
    .read = tty_read,
};

void fb_clear(fb_console_t *fb) {
    uint32_t* buffer = framebuffer_get_buffer();

    for (uint64_t y = 0; y < fb->height; y++)
        for (uint64_t x = 0; x < fb->width; x++)
            buffer[y * fb->pitch + x] = fb->bg;

    fb->cursor_x = 0;
    fb->cursor_y = 0;

    framebuffer_blit(buffer, fb->pixels, fb->width, fb->height);
}

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

void tty_init(tty_t *tty, void *backend,
              spinlock_t lock, uint32_t flags) {
    memset(tty, 0, sizeof(*tty));

    tty->flags   = flags;
    tty->backend = backend;
    tty->device.ops = &tty_inode_ops;
    tty->ops        = &tty_ops;
    tty->device.internal_data  = tty;

    spinlock_init(&lock);
    keyboard_register_listener(tty_input_listener);
}

void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, fb_console_t *fb, uint32_t flags) {
    spinlock_t framebuffer_lock = {0};

    backend->fb   = *fb;
    backend->ansi = (ansii_state_t){0};
    backend->fb_lock = framebuffer_lock;

    tty_init(tty, backend, backend->fb_lock, flags);
}

void tty_device_init(INode_t* tty_inode){
    file_t* f0 = kmalloc(sizeof(file_t));
    memset(f0, 0, sizeof(*f0));

    f0->inode = tty_inode;
    f0->flags = O_RDWR;
    f0->offset = 0;
    f0->shared = 1;
    current_fd_table->fds[0] = f0;
    current_fd_table->fds[1] = f0;
    current_fd_table->fds[2] = f0;
}