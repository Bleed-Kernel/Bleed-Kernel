#include <devices/devices.h>
#include <devices/type/tty_device.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <drivers/ps2/PS2_keyboard.h>
#include <input/keyboard_input.h>
#include <input/keyboard_dispatch.h>
#include <devices/device_io.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <mm/smap.h>
#include <console/console.h>
#include <fonts/utf-8.h>
#include <fonts/psf.h>
#include <stdio.h>
#include <stdbool.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/signal.h>
#include <user/errno.h>
#include <kernel/kmain.h>

typedef struct tty_linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} tty_linux_winsize_t;

#define MAX_TTY_DEVICES 12

static tty_t *tty_registry[MAX_TTY_DEVICES];
static size_t tty_registry_count = 0;
static bool tty_listener_registered = false;
extern struct INodeOps tty_inode_ops;

static tty_t *tty_get_by_index(uint32_t index) {
    if (index >= MAX_TTY_DEVICES)
        return NULL;
    return tty_registry[index];
}

static void tty_sync_termios_from_flags(tty_t *tty) {
    if (tty->flags & TTY_ECHO) tty->termios.c_lflag |= TTY_TERM_ECHO;
    else tty->termios.c_lflag &= ~TTY_TERM_ECHO;

    if (tty->flags & TTY_CANNONICAL) tty->termios.c_lflag |= TTY_TERM_ICANON;
    else tty->termios.c_lflag &= ~TTY_TERM_ICANON;

    if (tty->flags & TTY_NONBLOCK) {
        tty->termios.c_cc[TTY_VMIN] = 0;
        tty->termios.c_cc[TTY_VTIME] = 0;
    } else if (tty->termios.c_cc[TTY_VMIN] == 0 && tty->termios.c_cc[TTY_VTIME] == 0) {
        tty->termios.c_cc[TTY_VMIN] = 1;
    }
}

static void tty_sync_flags_from_termios(tty_t *tty) {
    tty->flags &= ~(TTY_ECHO | TTY_CANNONICAL | TTY_NONBLOCK);
    if (tty->termios.c_lflag & TTY_TERM_ECHO) tty->flags |= TTY_ECHO;
    if (tty->termios.c_lflag & TTY_TERM_ICANON) tty->flags |= TTY_CANNONICAL;
    if (tty->termios.c_cc[TTY_VMIN] == 0 && tty->termios.c_cc[TTY_VTIME] == 0)
        tty->flags |= TTY_NONBLOCK;
}

static void tty_fill_winsize(tty_t *tty, uint32_t *cols_out, uint32_t *rows_out) {
    tty_fb_backend_t *b = tty->backend;
    uint32_t cols = b->fb.width;
    uint32_t rows = b->fb.height;
    if (b->fb.font && b->fb.font->width) cols /= b->fb.font->width;
    if (b->fb.font && b->fb.font->height) rows /= b->fb.font->height;
    if (!cols) cols = 1;
    if (!rows) rows = 1;
    *cols_out = cols;
    *rows_out = rows;
}

static void tty_blit_to_display(tty_t *tty) {
    if (!tty || !tty->backend)
        return;

    tty_fb_backend_t *b = tty->backend;
    if (!b->display_pixels)
        return;
    if (b->fb.shadow_pixels && b->fb.shadow_initialized) {
        framebuffer_blit(b->fb.shadow_pixels, b->display_pixels, b->fb.width, b->fb.height, b->fb.pitch);
        return;
    }
    if (b->tty_pixels)
        framebuffer_blit(b->tty_pixels, b->display_pixels, b->fb.width, b->fb.height, b->fb.pitch);
}

static void tty_render_byte_shadow(tty_t *tty, uint8_t byte) {
    tty_fb_backend_t *b = tty->backend;

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

static void tty_drain_queued_output(tty_t *tty) {
    tty_fb_backend_t *b = tty->backend;
    if (!b || !b->is_active)
        return;
    if (tty->out_tail == tty->out_head)
        return;

    while (tty->out_tail != tty->out_head) {
        uint8_t c = (uint8_t)tty->outbuffer[tty->out_tail];
        tty->out_tail = (tty->out_tail + 1) % TTY_BUFFER_SZ;
        tty_render_byte_shadow(tty, c);
    }
}

long tty_read(INode_t *dev, void *buf, size_t len, size_t offset) {
    (void)offset;
    tty_t *tty = dev->internal_data;
    char *user_buf = (char *)buf;
    task_t *current = get_current_task();
    if (signal_should_interrupt(current))
        return -EINTR;

    unsigned long irq = irq_push();
    spinlock_acquire(&tty->in_lock);

    if (tty->flags & TTY_NONBLOCK) {
        if (tty->in_head == tty->in_tail) {
            spinlock_release(&tty->in_lock);
            irq_restore(irq);
            return -EAGAIN;
        }
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
        if (!has_line) {
            spinlock_release(&tty->in_lock);
            irq_restore(irq);
            return 0;
        }
    } else {
        if (tty->in_head == tty->in_tail && tty->termios.c_cc[TTY_VMIN] == 0) {
            spinlock_release(&tty->in_lock);
            irq_restore(irq);
            return 0;
        }
    }

    size_t bytes_read = 0;
    size_t min_required = 1;
    if (!(tty->flags & TTY_CANNONICAL)) {
        min_required = tty->termios.c_cc[TTY_VMIN];
        if (min_required == 0)
            min_required = 1;
    }

    while (bytes_read < len && tty->in_tail != tty->in_head) {
        if (signal_should_interrupt(current)) {
            spinlock_release(&tty->in_lock);
            irq_restore(irq);
            return bytes_read ? (long)bytes_read : -EINTR;
        }

        char c = tty->inbuffer[tty->in_tail];
        tty->in_tail = (tty->in_tail + 1) % TTY_BUFFER_SZ;
        user_buf[bytes_read++] = c;

        if ((tty->flags & TTY_CANNONICAL) && c == '\n') {
            break;
        }
        if (!(tty->flags & TTY_CANNONICAL) && bytes_read >= min_required) {
            break;
        }
    }

    spinlock_release(&tty->in_lock);
    irq_restore(irq);
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
                    tty_sync_termios_from_flags(tty);
                }
                return 0;
            
            case TTY_IOCTL_GET_FLAGS:
                tty_sync_flags_from_termios(tty);
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

            case TTY_IOCTL_GET_WINSIZE: {
                if (!arg) return -1;
                tty_winsize_t *ws = (tty_winsize_t *)arg;
                uint32_t cols = 1, rows = 1;
                tty_fill_winsize(tty, &cols, &rows);
                ws->cols = cols;
                ws->rows = rows;
                return 0;
            }

            case TTY_IOCTL_SET_CURSOR: {
                if (!arg) return -1;
                tty_fb_backend_t *b = tty->backend;
                tty_cursor_t *cursor = (tty_cursor_t *)arg;
                uint32_t cols = 1, rows = 1;
                tty_fill_winsize(tty, &cols, &rows);
                if (cursor->x < cols && cursor->y < rows) {
                    b->fb.cursor_x = cursor->x;
                    b->fb.cursor_y = cursor->y;
                }
                return 0;
            }

            case TTY_IOCTL_GET_INDEX:
                if (!arg) return -1;
                *(uint32_t *)arg = tty->index;
                return 0;

            case TTY_IOCTL_TCGETS:
                if (!arg) return -1;
                tty_sync_termios_from_flags(tty);
                *(tty_termios_t *)arg = tty->termios;
                return 0;

            case TTY_IOCTL_TCSETS:
            case TTY_IOCTL_TCSETSW:
            case TTY_IOCTL_TCSETSF:
                if (!arg) return -1;
                tty->termios = *(tty_termios_t *)arg;
                tty_sync_flags_from_termios(tty);
                if (req == TTY_IOCTL_TCSETSF) {
                    tty->in_tail = tty->in_head;
                    tty->line_start = tty->in_head;
                }
                return 0;

            case TTY_IOCTL_TIOCGWINSZ: {
                if (!arg) return -1;
                tty_linux_winsize_t *ws = (tty_linux_winsize_t *)arg;
                uint32_t cols = 1, rows = 1;
                tty_fill_winsize(tty, &cols, &rows);
                ws->ws_col = (uint16_t)cols;
                ws->ws_row = (uint16_t)rows;
                ws->ws_xpixel = 0;
                ws->ws_ypixel = 0;
                return 0;
            }

            case TTY_IOCTL_TIOCSWINSZ:
                return 0;

            case TTY_IOCTL_FIONBIO:
                if (!arg) return -1;
                if (*(int *)arg)
                    tty->flags |= TTY_NONBLOCK;
                else
                    tty->flags &= ~TTY_NONBLOCK;
                tty_sync_termios_from_flags(tty);
                return 0;

            case TTY_IOCTL_CREATE: {
                if (!arg) return -1;
                uint32_t index = 0;
                int rc = tty_create_framebuffer(&index);
                if (rc < 0)
                    return rc;
                *(uint32_t *)arg = index;
                return 0;
            }

            case TTY_IOCTL_SET_ACTIVE:
                if (!arg) return -1;
                return tty_set_active_index(*(uint32_t *)arg);

            case TTY_IOCTL_GET_ACTIVE_INDEX: {
                if (!arg) return -1;
                INode_t *active = console_get_active_console();
                if (!active || !active->internal_data)
                    return -ENOENT;
                tty_t *active_tty = (tty_t *)active->internal_data;
                *(uint32_t *)arg = active_tty->index;
                return 0;
            }

            default:
                return -1;
        }
    }
    return -1;
}

int tty_set_active_index(uint32_t index) {
    tty_t *tty = tty_get_by_index(index);
    if (!tty)
        return -ENOENT;

    INode_t *old_active_node = console_get_active_console();
    tty_t *old_active_tty = old_active_node ? (tty_t *)old_active_node->internal_data : NULL;
    if (old_active_tty && old_active_tty->backend) {
        tty_fb_backend_t *ob = (tty_fb_backend_t *)old_active_tty->backend;
        (void)ob;
    }

    for (size_t i = 0; i < tty_registry_count; i++) {
        if (!tty_registry[i] || !tty_registry[i]->backend)
            continue;
        ((tty_fb_backend_t *)tty_registry[i]->backend)->is_active = 0;
    }

    if (tty->backend) {
        tty_fb_backend_t *b = (tty_fb_backend_t *)tty->backend;
        if (!tty->activated_once) {
            b->fb.pixels = b->tty_pixels ? b->tty_pixels : b->fb.pixels;
            b->ansi = (ansii_state_t){0};
            b->utf8_len = 0;
            b->utf8_expected = 0;
            fb_clear(&b->fb);
            tty->activated_once = 1;
        }
        b->is_active = 1;
        if (b->display_pixels) {
            b->fb.pixels = b->display_pixels;
            tty_blit_to_display(tty);
        }
    }

    console_set(&tty->device, *tty);
    kernel_request_shell_spawn(tty->index);
    tty->out_tail = tty->out_head;
    tty_drain_queued_output(tty);
    return 0;
}

typedef struct tty_sig_pick_ctx {
    task_t *picked;
} tty_sig_pick_ctx_t;

static void tty_pick_sigint_target(task_t *candidate, void *userdata) {
    tty_sig_pick_ctx_t *ctx = (tty_sig_pick_ctx_t *)userdata;
    if (!candidate || !ctx || ctx->picked)
        return;
    if (candidate->task_privilege != PRIVILEGE_USER)
        return;
    if (candidate->state == TASK_DEAD || candidate->state == TASK_FREE || candidate->state == TASK_ZOMBIE)
        return;
    ctx->picked = candidate;
}

static int tty_keycode_to_fn_number(uint16_t keycode) {
    if (keycode >= 59 && keycode <= 68)
        return (int)(keycode - 58); // F1..F10
    if (keycode == 87)
        return 11; // F11
    if (keycode == 88)
        return 12; // F12
    return 0;
}

static void tty_input_listener(const keyboard_event_t *ev) {
    if (ev->action != KEY_DOWN)
        return;

    int fn = tty_keycode_to_fn_number(ev->keycode);

    if (fn > 0 &&
        ((ev->keymod & KEYMOD_ALT) || (ev->keymod & KEYMOD_CTRL))) {
        uint32_t target = (uint32_t)(fn - 1);
        while (tty_registry_count <= target) {
            if (tty_create_framebuffer(NULL) < 0)
                break;
        }
        if (target < tty_registry_count)
            (void)tty_set_active_index(target);
        return;
    }

    INode_t *active = console_get_active_console();
    if (!active || !active->internal_data)
        return;

    tty_t *tty = (tty_t *)active->internal_data;

    char c = tty_key_to_ascii(ev);
    if (!c)
        return;

    if ((ev->keymod & KEYMOD_CTRL) && (c == 'c' || c == 'C')) {
        if (!(tty->termios.c_lflag & TTY_TERM_ISIG))
            return;

        task_t *task = get_current_task();
        if (!task || task->task_privilege != PRIVILEGE_USER) {
            tty_sig_pick_ctx_t ctx = {0};
            itterate_each_task(tty_pick_sigint_target, &ctx);
            task = ctx.picked;
        }

        if (task)
            (void)signal_send(task, SIGINT);

        if (tty->flags & TTY_ECHO) {
            tty->ops->putchar(tty, '^');
            tty->ops->putchar(tty, 'C');
            tty->ops->putchar(tty, '\n');
        }
        tty->in_head = tty->line_start;
        return;
    }

    tty_process_input(tty, c);
}

static void tty_fb_putchar(tty_t *tty, char c) {
    tty_fb_backend_t *b = tty->backend;
    uint8_t byte = (uint8_t)c;
    if (!b)
        return;

    if (!b->is_active) {
        tty_render_byte_shadow(tty, byte);
        return;
    }

    tty_render_byte_shadow(tty, byte);
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
    if (!fb || !fb->pixels)
        return;

    uint32_t *shadow = fb->shadow_pixels ? fb->shadow_pixels : fb->pixels;
    for (uint64_t y = 0; y < fb->height; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            shadow[y * fb->pitch + x] = fb->bg;
        }
    }

    if (shadow != fb->pixels) {
        framebuffer_blit(shadow, fb->pixels, fb->width, fb->height, fb->pitch);
    }

    fb->cursor_x = 0;
    fb->cursor_y = 0;
    fb->dirty_top = fb->height;
    fb->dirty_bottom = 0;
}

void tty_process_input(tty_t *tty, char c) {
    if ((tty->flags & TTY_CANNONICAL) && c == '\r') c = '\n';
    unsigned long irq = irq_push();
    spinlock_acquire(&tty->in_lock);

    if ((tty->flags & TTY_CANNONICAL) && (c == '\b' || c == tty->termios.c_cc[TTY_VERASE])) {
        if (tty->in_head != tty->line_start) {
            tty->in_head = (tty->in_head + TTY_BUFFER_SZ - 1) % TTY_BUFFER_SZ;
            if (tty->flags & TTY_ECHO) {
                tty->ops->putchar(tty, '\b');
                tty->ops->putchar(tty, ' ');
                tty->ops->putchar(tty, '\b');
            }
        }
        spinlock_release(&tty->in_lock);
        irq_restore(irq);
        return;
    }

    size_t next = (tty->in_head + 1) % TTY_BUFFER_SZ;
    if (next == tty->in_tail) {
        spinlock_release(&tty->in_lock);
        irq_restore(irq);
        return;
    }

    tty->inbuffer[tty->in_head] = c;
    tty->in_head = next;
    if ((tty->flags & TTY_CANNONICAL) && c == '\n') {
        tty->line_start = tty->in_head;
    }

    if (tty->flags & TTY_ECHO) {
        tty->ops->putchar(tty, c);
    }

    spinlock_release(&tty->in_lock);
    irq_restore(irq);
}

void tty_init(tty_t *tty, void *backend, spinlock_t lock, uint32_t flags) {
    (void)lock;
    memset(tty, 0, sizeof(*tty));
    tty->flags = flags;
    tty->index = tty_registry_count;
    tty->backend = backend;
    tty->device.ops = &tty_inode_ops;
    tty->ops = &tty_ops;
    tty->device.internal_data = tty;
    tty->termios.c_lflag = TTY_TERM_ECHO | TTY_TERM_ICANON | TTY_TERM_ISIG;
    tty->termios.c_cc[TTY_VINTR] = 3;   // ^C
    tty->termios.c_cc[TTY_VERASE] = 127;
    tty->termios.c_cc[TTY_VMIN] = 1;
    tty->termios.c_cc[TTY_VTIME] = 0;
    tty_sync_flags_from_termios(tty);
    tty->flags |= (flags & (TTY_ECHO | TTY_CANNONICAL | TTY_NONBLOCK));
    tty_sync_termios_from_flags(tty);
    spinlock_init(&tty->in_lock);
    if (tty_registry_count < MAX_TTY_DEVICES) {
        tty_registry[tty_registry_count++] = tty;
    }

    if (!tty_listener_registered) {
        keyboard_register_listener(tty_input_listener);
        tty_listener_registered = true;
    }
}

void tty_init_framebuffer(tty_t *tty, tty_fb_backend_t *backend, fb_console_t *fb, uint32_t flags) {
    backend->fb = *fb;
    backend->fb.cursor_x = 0;
    backend->fb.cursor_y = 0;
    backend->fb.shadow_pixels = NULL;
    backend->fb.shadow_pixels_size = 0;
    backend->fb.shadow_initialized = 0;
    backend->fb.dirty_top = backend->fb.height;
    backend->fb.dirty_bottom = 0;
    backend->display_pixels = fb->pixels;
    backend->is_active = 0;
    size_t pixels = (size_t)fb->pitch * (size_t)fb->height;
    backend->tty_pixels = kmalloc(pixels * sizeof(uint32_t));
    if (backend->tty_pixels) {
        for (size_t i = 0; i < pixels; i++)
            backend->tty_pixels[i] = fb->bg;
        backend->fb.pixels = backend->tty_pixels;
    }
    backend->ansi = (ansii_state_t){0};
    backend->fb_lock = (spinlock_t){0};
    backend->utf8_len = 0;
    backend->utf8_expected = 0;
    tty_init(tty, backend, backend->fb_lock, flags);
}

int tty_create_framebuffer(uint32_t *out_index) {
    if (tty_registry_count >= MAX_TTY_DEVICES)
        return -ENOSPC;

    tty_t *tty = kmalloc(sizeof(tty_t));
    tty_fb_backend_t *backend = kmalloc(sizeof(tty_fb_backend_t));
    char *name = kmalloc(16);
    if (!tty || !backend || !name)
        return -ENOMEM;

    uint32_t next_index = (uint32_t)tty_registry_count;
    fb_console_t fb = {
        .pixels = framebuffer_get_addr(0),
        .width = framebuffer_get_width(0),
        .height = framebuffer_get_height(0),
        .pitch = framebuffer_get_pitch(0) / 4,
        .font = psf_get_current_font(),
        .fg = 0xFFFFFF,
        .bg = 0x000000,
    };

    tty_init_framebuffer(tty, backend, &fb, TTY_ECHO | TTY_CANNONICAL);
    snprintf(name, 16, "tty%u", next_index);
    int rc = (int)device_register(&tty->device, name);
    if (rc < 0)
        return rc;

    if (out_index)
        *out_index = tty->index;
    return 0;
}

void tty_device_init(INode_t *tty_inode) {
    file_t *f0 = kmalloc(sizeof(file_t));
    memset(f0, 0, sizeof(*f0));
    f0->inode = tty_inode;
    f0->flags = O_RDWR;
    f0->offset = 0;
    f0->shared = 2;

    fd_table_t *boot_fds = vfs_get_kernel_table();
    if (!boot_fds) {
        kfree(f0, sizeof(*f0));
        return;
    }

    boot_fds->fds[1] = f0;
    boot_fds->fds[2] = f0;
}
