#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <drivers/serial/serial.h>
#include <input/keyboard_input.h>
#include <input/keyboard_dispatch.h>
#include <devices/device_io.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <mm/smap.h>
#include <console/console.h>
#include <fonts/utf-8.h>
#include <stdio.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/signal.h>
#include <user/errno.h>

long tty_read(INode_t *dev, void *buf, size_t len, size_t offset) {
    (void)offset;
    tty_t *tty = dev->internal_data;
    char *user_buf = (char *)buf;

    if (tty->flags & TTY_NONBLOCK) {
        if (tty->in_head == tty->in_tail) {
            return -EAGAIN;
        }
        return len;
    }

    if (tty->flags & TTY_CANNONICAL) {
        int has_line = 0;
        size_t i = tty->in_tail;
        while (i != tty->in_head) {
            if (tty->inbuffer[i] == '\n') {
                has_line = 1;
                break;
            }
            i = (i + 1) % TTY_BUFFER_SZ;
        }
        if (!has_line) return 0;
    }

    size_t bytes_read = 0;
    while (bytes_read < len && tty->in_tail != tty->in_head) {
        char c = tty->inbuffer[tty->in_tail];
        tty->in_tail = (tty->in_tail + 1) % TTY_BUFFER_SZ;
        user_buf[bytes_read++] = c;

        if ((tty->flags & TTY_CANNONICAL) && c == '\n') {
            break;
        }
    }

    return bytes_read;
}

long tty_inode_write(INode_t *inode, const void *in_buffer, size_t size, size_t offset) {
    (void)offset;
    tty_t *tty = inode->internal_data;
    const uint8_t *c = in_buffer;
    for (size_t i = 0; i < size; i++)
        tty->ops->putchar(tty, (char)c[i]);
    return size;
}

int tty_ioctl(INode_t *dev, unsigned long req, void *arg) {
    tty_t *tty = dev->internal_data;
    uint32_t *user_flags = arg;

    SMAP_ALLOW{
        switch (req) {
            case TTY_IOCTL_SET_FLAGS:
                if (tty->flags != *user_flags) {
                    tty->flags = *user_flags;
                }
                return 0;
            
            case TTY_IOCTL_GET_FLAGS:
                *user_flags = tty->flags;
                return 0;

            case TTY_IOCTL_GET_CURSOR: {
                if (!arg) return -1;
                tty_fb_backend_t *b = tty->backend;
                tty_cursor_t *cursor = (tty_cursor_t *)arg;
                cursor->x = b->fb.cursor_x;
                cursor->y = b->fb.cursor_y;
                return 0;
            }

            case TTY_IOCTL_SET_CURSOR: {
                if (!arg) return -1;
                tty_fb_backend_t *b = tty->backend;
                tty_cursor_t *cursor = (tty_cursor_t *)arg;
                if (cursor->x < b->fb.width && cursor->y < b->fb.height) {
                    b->fb.cursor_x = cursor->x;
                    b->fb.cursor_y = cursor->y;
                }
                return 0;
            }

            default:
                return -1;
        }
    }
    return -1;
}

static void tty_pick_sigint_target(task_t *candidate, void *userdata) {
    if (!candidate || !userdata)
        return;
    if (candidate->task_privilege != PRIVILEGE_USER)
        return;
    if (candidate->state == TASK_DEAD || candidate->state == TASK_FREE)
        return;

    task_t *ctx = (task_t *)userdata;
    if (!ctx)
        ctx = candidate;
}

static void tty_input_listener(const keyboard_event_t *ev) {
    if (ev->action != KEY_DOWN)
        return;

    tty_t *tty = (tty_t *)console_get_active_console()->internal_data;

    char c = tty_key_to_ascii(ev);
    if (!c)
        return;

    if ((ev->keymod & KEYMOD_CTRL) && (c == 'c' || c == 'C')) {
        task_t *task = get_current_task();
        if (!task || task->task_privilege != PRIVILEGE_USER) {
            cpu_context_t ctx = {0};
            itterate_each_task(tty_pick_sigint_target, &ctx);
            task->context = &ctx;
        }

        if (task)
            (void)signal_send(task, SIGINT);

        if (tty->flags & TTY_ECHO) {
            tty->ops->putchar(tty, '^');
            tty->ops->putchar(tty, 'C');
            tty->ops->putchar(tty, '\n');
        }
        return;
    }

    tty_process_input(tty, c);
}

static void tty_fb_putchar(tty_t *tty, char c) {
    tty_fb_backend_t *b = tty->backend;
    uint8_t byte = (uint8_t)c;

    if (b->utf8_len == 0) {
        if (byte < 0x80) {
            framebuffer_ansi_char(&b->fb, &b->fb_lock, &b->ansi, (uint32_t)byte);
            return;
        } else if ((byte & 0xE0) == 0xC0) b->utf8_expected = 2;
        else if ((byte & 0xF0) == 0xE0) b->utf8_expected = 3;
        else if ((byte & 0xF8) == 0xF0) b->utf8_expected = 4;
        else return;
        b->utf8_buf[b->utf8_len++] = byte;
    } else {
        b->utf8_buf[b->utf8_len++] = byte;
        if (b->utf8_len == b->utf8_expected) {
            uint32_t codepoint = 0;
            utf8_decode(b->utf8_buf, &codepoint);
            framebuffer_ansi_char(&b->fb, &b->fb_lock, &b->ansi, codepoint);
            b->utf8_len = 0;
            b->utf8_expected = 0;
        }
    }
}

struct tty_ops tty_ops = {
    .putchar = tty_fb_putchar 
};

struct INodeOps tty_inode_ops = {
    .write = tty_inode_write,
    .read = tty_read,
    .ioctl = tty_ioctl
};

void fb_clear(fb_console_t *fb) {
    uint32_t *buffer = framebuffer_get_buffer();
    for (uint64_t y = 0; y < fb->height; y++)
        for (uint64_t x = 0; x < fb->width; x++)
            buffer[y * fb->pitch + x] = fb->bg;
    fb->cursor_x = 0;
    fb->cursor_y = 0;
    framebuffer_blit(buffer, fb->pixels, fb->width, fb->height);
}

void tty_process_input(tty_t *tty, char c) {
    if (c == '\r') c = '\n';

    if (c == '\b' || c == 127) {
        if (tty->in_head != tty->in_tail) {
            tty->in_head = (tty->in_head + TTY_BUFFER_SZ - 1) % TTY_BUFFER_SZ;
            if (tty->flags & TTY_ECHO) {
                tty->ops->putchar(tty, '\b');
                tty->ops->putchar(tty, ' ');
                tty->ops->putchar(tty, '\b');
            }
        }
        return;
    }

    size_t next = (tty->in_head + 1) % TTY_BUFFER_SZ;
    if (next == tty->in_tail) return; 

    tty->inbuffer[tty->in_head] = c;
    tty->in_head = next;

    if (tty->flags & TTY_ECHO) {
        tty->ops->putchar(tty, c);
    }
}

void tty_init(tty_t *tty, void *backend, spinlock_t lock, uint32_t flags) {
    memset(tty, 0, sizeof(*tty));
    tty->flags = flags;
    tty->backend = backend;
    tty->device.ops = &tty_inode_ops;
    tty->ops = &tty_ops;
    tty->device.internal_data = tty;
    spinlock_init(&lock);
    keyboard_register_listener(tty_input_listener);
}

void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, fb_console_t *fb, uint32_t flags) {
    backend->fb = *fb;
    backend->ansi = (ansii_state_t){0};
    backend->fb_lock = (spinlock_t){0};
    backend->utf8_len = 0;
    backend->utf8_expected = 0;
    tty_init(tty, backend, backend->fb_lock, flags);
}

void tty_device_init(INode_t *tty_inode) {
    file_t *f0 = kmalloc(sizeof(file_t));
    memset(f0, 0, sizeof(*f0));
    f0->inode = tty_inode;
    f0->flags = O_RDWR;
    f0->offset = 0;
    f0->shared = 1;
    current_fd_table->fds[1] = f0;
    current_fd_table->fds[2] = f0;
}