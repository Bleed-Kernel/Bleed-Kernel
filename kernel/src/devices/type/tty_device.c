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
#include <user/user_copy.h>
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
#include <stdio.h>

typedef struct tty_linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} tty_linux_winsize_t;

static tty_t           g_tty0;
static tty_fb_backend_t g_tty0_backend;
static bool            g_tty0_ready           = false;
static bool            g_listener_registered  = false;

extern struct INodeOps tty_inode_ops;
extern spinlock_t sched_lock;

static void tty_sync_termios_from_flags(tty_t *tty) {
    if (tty->flags & TTY_ECHO)      tty->termios.c_lflag |=  TTY_TERM_ECHO;
    else                            tty->termios.c_lflag &= ~TTY_TERM_ECHO;

    if (tty->flags & TTY_CANNONICAL) tty->termios.c_lflag |=  TTY_TERM_ICANON;
    else                             tty->termios.c_lflag &= ~TTY_TERM_ICANON;

    if (tty->flags & TTY_NONBLOCK) {
        tty->termios.c_cc[TTY_VMIN]  = 0;
        tty->termios.c_cc[TTY_VTIME] = 0;
    } else if (tty->termios.c_cc[TTY_VMIN] == 0 && tty->termios.c_cc[TTY_VTIME] == 0) {
        tty->termios.c_cc[TTY_VMIN] = 1;
    }
}

static void tty_sync_flags_from_termios(tty_t *tty) {
    tty->flags &= ~(TTY_ECHO | TTY_CANNONICAL | TTY_NONBLOCK);
    if (tty->termios.c_lflag & TTY_TERM_ECHO)   tty->flags |= TTY_ECHO;
    if (tty->termios.c_lflag & TTY_TERM_ICANON) tty->flags |= TTY_CANNONICAL;
    if (tty->termios.c_cc[TTY_VMIN] == 0 && tty->termios.c_cc[TTY_VTIME] == 0)
        tty->flags |= TTY_NONBLOCK;
}

static void tty_fill_winsize(tty_t *tty, uint32_t *cols_out, uint32_t *rows_out) {
    tty_fb_backend_t *b = tty->backend;
    uint32_t cols = b->fb.width;
    uint32_t rows = b->fb.height;
    if (b->fb.font && b->fb.font->width)  cols /= b->fb.font->width;
    if (b->fb.font && b->fb.font->height) rows /= b->fb.font->height;
    if (!cols) cols = 1;
    if (!rows) rows = 1;
    *cols_out = cols;
    *rows_out = rows;
}

static void tty_render_byte(tty_t *tty, uint8_t byte) {
    tty_fb_backend_t *b = tty->backend;

    if (b->utf8_len == 0) {
        if (byte < 0x80) {
            framebuffer_ansi_char(&b->fb, &b->fb_lock, &b->ansi, (uint32_t)byte);
            return;
        } else if ((byte & 0xE0) == 0xC0) b->utf8_expected = 2;
        else if ((byte & 0xF0) == 0xE0)   b->utf8_expected = 3;
        else if ((byte & 0xF8) == 0xF0)   b->utf8_expected = 4;
        else return;
        b->utf8_buf[b->utf8_len++] = byte;
    } else {
        b->utf8_buf[b->utf8_len++] = byte;
        if (b->utf8_len == b->utf8_expected) {
            uint32_t codepoint = 0;
            utf8_decode(b->utf8_buf, &codepoint);
            framebuffer_ansi_char(&b->fb, &b->fb_lock, &b->ansi, codepoint);
            b->utf8_len     = 0;
            b->utf8_expected = 0;
        }
    }
}

static void tty_fb_putchar(tty_t *tty, char c) {
    if (!tty->backend) return;
    tty_render_byte(tty, (uint8_t)c);
}

void fb_clear(fb_console_t *fb) {
    if (!fb || !fb->pixels) return;

    uint32_t *dst = fb->shadow_pixels ? fb->shadow_pixels : fb->pixels;
    for (uint64_t y = 0; y < fb->height; y++)
        for (uint64_t x = 0; x < fb->width; x++)
            dst[y * fb->pitch + x] = fb->bg;

    if (dst != fb->pixels)
        framebuffer_blit(dst, fb->pixels, fb->width, fb->height, fb->pitch);

    fb_scrollback_reset(fb);

    fb->cursor_x     = 0;
    fb->cursor_y     = 0;
    fb->dirty_top    = fb->height;
    fb->dirty_bottom = 0;
}

void tty_process_input(tty_t *tty, char c) {
    if ((tty->flags & TTY_CANNONICAL) && c == '\r') c = '\n';

    unsigned long irq = irq_push();
    spinlock_acquire(&tty->in_lock);

    /* backspace in canonical mode */
    if ((tty->flags & TTY_CANNONICAL) &&
        (c == '\b' || c == tty->termios.c_cc[TTY_VERASE])) {
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

    if ((tty->flags & TTY_CANNONICAL) && c == '\n')
        tty->line_start = tty->in_head;

    if (tty->flags & TTY_ECHO)
        tty->ops->putchar(tty, c);

    spinlock_release(&tty->in_lock);
    irq_restore(irq);
}

typedef struct { task_t *picked; } tty_sig_pick_ctx_t;

static void tty_pick_sigint_target(task_t *candidate, void *userdata) {
    tty_sig_pick_ctx_t *ctx = (tty_sig_pick_ctx_t *)userdata;
    if (!candidate || !ctx) return;
    if (candidate->task_privilege != PRIVILEGE_USER) return;
    if (candidate->state == TASK_DEAD ||
        candidate->state == TASK_FREE ||
        candidate->state == TASK_ZOMBIE) return;
    if (!ctx->picked || candidate->id > ctx->picked->id)
        ctx->picked = candidate;
}

static void tty_input_listener(const keyboard_event_t *ev) {
    if (ev->action != KEY_DOWN) return;
    if (!g_tty0_ready) return;

    tty_t *tty = &g_tty0;

    char c = tty_key_to_ascii(ev);
    if (!c) return;

    if ((ev->keymod & KEYMOD_CTRL) && (c == 'c' || c == 'C')) {
        if (!(tty->termios.c_lflag & TTY_TERM_ISIG)) return;

        tty_sig_pick_ctx_t ctx = {0};
        itterate_each_task(tty_pick_sigint_target, &ctx);
        if (ctx.picked)
            signal_send(ctx.picked, SIGINT);

        if (tty->flags & TTY_ECHO) {
            tty->ops->putchar(tty, '^');
            tty->ops->putchar(tty, 'C');
            tty->ops->putchar(tty, '\n');
        }

        unsigned long irq = irq_push();
        spinlock_acquire(&tty->in_lock);
        tty->in_head = tty->line_start;
        spinlock_release(&tty->in_lock);
        irq_restore(irq);
        return;
    }

    tty_process_input(tty, c);
}

long tty_read(INode_t *dev, void *buf, size_t len, size_t offset) {
    (void)offset;
    tty_t *tty = dev->internal_data;
    char  *user_buf = (char *)buf;
    task_t *current = get_current_task();

    if (signal_should_interrupt(current)) return -EINTR;

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
            if (tty->inbuffer[i] == '\n') { has_line = 1; break; }
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

    size_t bytes_read   = 0;
    size_t min_required = (tty->flags & TTY_CANNONICAL) ? 1 : tty->termios.c_cc[TTY_VMIN];
    if (min_required == 0) min_required = 1;

    // no signal check in here, a fatal one never returns and would take in_lock with it.
    // the loop cant block anyway, the check before the lock is enough
    while (bytes_read < len && tty->in_tail != tty->in_head) {
        char c = tty->inbuffer[tty->in_tail];
        tty->in_tail = (tty->in_tail + 1) % TTY_BUFFER_SZ;
        user_buf[bytes_read++] = c;

        if ((tty->flags & TTY_CANNONICAL) && c == '\n') break;
        if (!(tty->flags & TTY_CANNONICAL) && bytes_read >= min_required) break;
    }

    spinlock_release(&tty->in_lock);
    irq_restore(irq);
    return (long)bytes_read;
}

long tty_inode_write(INode_t *inode, const void *in_buffer, size_t size, size_t offset) {
    (void)offset;
    tty_t *tty = inode->internal_data;
    const uint8_t *c = in_buffer;
    for (size_t i = 0; i < size; i++)
        tty->ops->putchar(tty, (char)c[i]);
    return (long)size;
}

// arg is a raw user pointer, everything in and out of an ioctl goes through these two
static int tty_arg_in(void *arg, void *dst, size_t len) {
    if (!arg) return -EINVAL;
    return copy_from_user(get_current_task(), dst, arg, len) == 0 ? 0 : -EFAULT;
}

static int tty_arg_out(void *arg, const void *src, size_t len) {
    if (!arg) return -EINVAL;
    return copy_to_user(get_current_task(), arg, src, len) == 0 ? 0 : -EFAULT;
}

int tty_ioctl(INode_t *dev, unsigned long req, void *arg) {
    tty_t *tty = dev->internal_data;
    tty_fb_backend_t *b = tty->backend;
    int r;

    switch (req) {
        case TTY_IOCTL_SET_FLAGS: {
            uint32_t flags;
            if ((r = tty_arg_in(arg, &flags, sizeof(flags))) < 0) return r;
            tty->flags = flags;
            tty_sync_termios_from_flags(tty);
            return 0;
        }

        case TTY_IOCTL_GET_FLAGS:
            if (!arg) return -EINVAL;
            tty_sync_flags_from_termios(tty);
            return tty_arg_out(arg, &tty->flags, sizeof(tty->flags));

        case TTY_IOCTL_GET_CURSOR: {
            tty_cursor_t cursor = {
                .x = b->fb.cursor_x,
                .y = b->fb.cursor_y,
            };
            return tty_arg_out(arg, &cursor, sizeof(cursor));
        }

        case TTY_IOCTL_SET_CURSOR: {
            tty_cursor_t cursor;
            if ((r = tty_arg_in(arg, &cursor, sizeof(cursor))) < 0) return r;

            uint32_t cols = 1, rows = 1;
            tty_fill_winsize(tty, &cols, &rows);
            if (cursor.x < cols && cursor.y < rows) {
                b->fb.cursor_x = cursor.x;
                b->fb.cursor_y = cursor.y;
            }
            return 0;
        }

        case TTY_IOCTL_GET_WINSIZE: {
            tty_winsize_t ws;
            tty_fill_winsize(tty, &ws.cols, &ws.rows);
            return tty_arg_out(arg, &ws, sizeof(ws));
        }

        case TTY_IOCTL_GET_INDEX: {
            uint32_t index = 0;
            return tty_arg_out(arg, &index, sizeof(index));
        }

        case TTY_IOCTL_TCGETS:
            if (!arg) return -EINVAL;
            tty_sync_termios_from_flags(tty);
            return tty_arg_out(arg, &tty->termios, sizeof(tty->termios));

        case TTY_IOCTL_TCSETS:
        case TTY_IOCTL_TCSETSW:
        case TTY_IOCTL_TCSETSF: {
            tty_termios_t term;
            if ((r = tty_arg_in(arg, &term, sizeof(term))) < 0) return r;

            tty->termios = term;
            tty_sync_flags_from_termios(tty);
            if (req == TTY_IOCTL_TCSETSF) {
                tty->in_tail    = tty->in_head;
                tty->line_start = tty->in_head;
            }
            return 0;
        }

        case TTY_IOCTL_TIOCGWINSZ: {
            uint32_t cols = 1, rows = 1;
            tty_fill_winsize(tty, &cols, &rows);

            tty_linux_winsize_t ws = {
                .ws_row = (uint16_t)rows,
                .ws_col = (uint16_t)cols,
            };
            return tty_arg_out(arg, &ws, sizeof(ws));
        }

        case TTY_IOCTL_TIOCSWINSZ:
            return 0;

        case TTY_IOCTL_FIONBIO: {
            int nonblock;
            if ((r = tty_arg_in(arg, &nonblock, sizeof(nonblock))) < 0) return r;

            if (nonblock) tty->flags |=  TTY_NONBLOCK;
            else          tty->flags &= ~TTY_NONBLOCK;
            tty_sync_termios_from_flags(tty);
            return 0;
        }

        case TTY_IOCTL_SCROLL: {
            int lines_to_scroll;
            if ((r = tty_arg_in(arg, &lines_to_scroll, sizeof(lines_to_scroll))) < 0) return r;

            unsigned long irq = irq_push();
            spinlock_acquire(&b->fb_lock);

            fb_console_scroll(&b->fb, lines_to_scroll);
            int view = (int)b->fb.scrollback_view;

            spinlock_release(&b->fb_lock);
            irq_restore(irq);

            // copied out after the lock is gone, the copy can fault
            return tty_arg_out(arg, &view, sizeof(view));
        }

        default:
            return -ENOTTY;
    }
}

static struct tty_ops tty0_ops = {
    .putchar = tty_fb_putchar,
};

struct INodeOps tty_inode_ops = {
    .write = tty_inode_write,
    .read  = tty_read,
    .ioctl = tty_ioctl,
};

void tty_device_init(void) {
    tty_t           *tty = &g_tty0;
    tty_fb_backend_t *b  = &g_tty0_backend;

    fb_console_t fb = {
        .pixels = framebuffer_get_addr(0),
        .width  = framebuffer_get_width(0),
        .height = framebuffer_get_height(0),
        .pitch  = framebuffer_get_pitch(0) / (framebuffer_get_bpp(0) / 8),
        .font   = psf_get_current_font(),
        .fg     = 0xFFFFFF,
        .bg     = 0x000000,
    };

    memset(b, 0, sizeof(*b));
    b->fb              = fb;
    b->display_pixels  = fb.pixels;
    b->is_active       = 1;
    b->ansi            = (ansii_state_t){0};
    b->fb_lock         = (spinlock_t){0};

    memset(tty, 0, sizeof(*tty));
    tty->index   = 0;
    tty->backend = b;
    tty->device.ops           = &tty_inode_ops;
    tty->device.internal_data = tty;
    tty->device.shared        = 1;
    tty->ops     = &tty0_ops;

    tty->flags = TTY_ECHO | TTY_CANNONICAL;
    tty->termios.c_lflag             = TTY_TERM_ECHO | TTY_TERM_ICANON | TTY_TERM_ISIG;
    tty->termios.c_cc[TTY_VINTR]     = 3;
    tty->termios.c_cc[TTY_VERASE]    = 127;
    tty->termios.c_cc[TTY_VMIN]      = 1;
    tty->termios.c_cc[TTY_VTIME]     = 0;
    tty_sync_termios_from_flags(tty);
    spinlock_init(&tty->in_lock);

    device_register(&tty->device, "tty0");

    file_t *f = kmalloc(sizeof(file_t));
    if (f) {
        memset(f, 0, sizeof(*f));
        f->type   = FD_TYPE_DEV;
        f->inode  = &tty->device;
        f->flags  = O_RDWR;
        f->shared = 2;
        fd_table_t *boot_fds = vfs_get_kernel_table();
        if (boot_fds) {
            f->inode->shared++;
            boot_fds->fds[1] = f;
            boot_fds->fds[2] = f;
        } else {
            kfree(f);
        }
    }

    console_set(&tty->device, *tty);

    if (!g_listener_registered) {
        keyboard_register_listener(tty_input_listener);
        g_listener_registered = true;
    }

    g_tty0_ready = true;
}