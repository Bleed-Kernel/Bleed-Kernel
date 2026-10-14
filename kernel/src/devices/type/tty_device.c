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
#include <user/user_poll.h>
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

static tty_t          *g_offscreen;             // tty1 and up. one is never taken off, only given back to be used again
static spinlock_t      g_offscreen_lock       = {0};
static tty_t          *g_active_offscreen;      // the one whose window has the focus, NULL when none does

extern struct INodeOps tty_inode_ops;
extern spinlock_t sched_lock;
static struct tty_ops tty0_ops;

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

    // its window is gone, whatever still has it open is talking to nobody
    if (tty->offscreen && !tty->owner_pid) return;
    tty_render_byte(tty, (uint8_t)c);
}

void fb_clear(fb_console_t *fb) {
    if (!fb || !fb->pixels) return;

    uint32_t *dst = fb->shadow_pixels ? fb->shadow_pixels : fb->pixels;
    for (uint64_t y = 0; y < fb->height; y++)
        for (uint64_t x = 0; x < fb->width; x++)
            dst[y * fb->pitch + x] = fb->bg;

    if (dst != fb->pixels && !fb->detached)
        framebuffer_blit(dst, fb->pixels, fb->width, fb->height, fb->pitch);
    framebuffer_mark_grab(fb, 0, fb->height);

    fb_scrollback_reset(fb);
    fb_text_clear(fb);

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

    sched_wake_event_waiters();
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

    // whoever took the screen reads the keyboard device itself, no echo and no ^C from us
    if (g_tty0_backend.fb.detached) return;

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

    // whoever is showing it in a window sleeps in poll until theres something new
    if (tty->offscreen)
        sched_wake_event_waiters();
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

static void tty_offscreen_free_buffers(tty_fb_backend_t *b) {
    if (b->fb.pixels)           kfree(b->fb.pixels);
    if (b->fb.shadow_pixels)    kfree(b->fb.shadow_pixels);
    if (b->fb.scrollback_lines) kfree(b->fb.scrollback_lines);
    fb_text_detach(&b->fb);

    b->fb.pixels           = NULL;
    b->fb.shadow_pixels    = NULL;
    b->fb.scrollback_lines = NULL;
}

static void tty_offscreen_destroy(tty_t *tty) {
    tty_fb_backend_t *b = tty->backend;

    unsigned long irq = irq_push();
    spinlock_acquire(&b->fb_lock);

    // the tty_t and its /dev node stay for the next window, only the pixels go back
    tty->owner_pid = 0;
    if (g_active_offscreen == tty)
        g_active_offscreen = NULL;
    tty_offscreen_free_buffers(b);

    spinlock_release(&b->fb_lock);
    irq_restore(irq);
}

// the tty a task writes to, going by its stdout and then its stderr
static tty_t *tty_of_task(task_t *task) {
    if (!task || !task->fd_table) return NULL;

    for (int fd = 1; fd <= 2; fd++) {
        file_t *f = task->fd_table->fds[fd];
        if (f && f->inode && f->inode->ops == &tty_inode_ops && f->inode->internal_data)
            return (tty_t *)f->inode->internal_data;
    }
    return NULL;
}

static uint32_t tty_offscreen_grow(uint32_t n) {
    return (n + TTY_OFFSCREEN_GROW - 1) / TTY_OFFSCREEN_GROW * TTY_OFFSCREEN_GROW;
}

// SIGWINCH for everything that has this tty as its terminal
static void tty_send_winch(task_t *task, void *userdata) {
    if (!task || task->task_privilege != PRIVILEGE_USER) return;
    if (tty_of_task(task) == (tty_t *)userdata)
        signal_send(task, SIGWINCH);
}

/// @brief give an offscreen tty a different size in pixels and wrap its text again to fit
/// @return 0 on success, negative status code on failure
static int tty_offscreen_resize(tty_t *tty, uint32_t width, uint32_t height) {
    tty_fb_backend_t *b  = tty->backend;
    fb_console_t     *fb = &b->fb;
    if (width < TTY_OFFSCREEN_MIN || height < TTY_OFFSCREEN_MIN ||
        width > TTY_OFFSCREEN_MAX || height > TTY_OFFSCREEN_MAX)
        return -EINVAL;
    if (width == fb->width && height == fb->height)
        return 0;

    // only allocated if it has outgrown what it has, and before the lock is taken
    uint32_t  cap_w  = fb->pitch;
    uint32_t  cap_h  = b->cap_height;
    uint32_t *pixels = NULL;
    uint32_t *shadow = NULL;
    size_t    bytes  = 0;

    if (width > cap_w || height > cap_h) {
        cap_w = tty_offscreen_grow(width > cap_w ? width : cap_w);
        cap_h = tty_offscreen_grow(height > cap_h ? height : cap_h);
        bytes = (size_t)cap_w * cap_h * sizeof(uint32_t);

        pixels = kmalloc(bytes);
        shadow = kmalloc(bytes);
        if (!pixels || !shadow) {
            if (pixels) kfree(pixels);
            if (shadow) kfree(shadow);
            return -ENOMEM;
        }
    }

    if (fb_text_spare_alloc(fb) != 0) {
        if (pixels) kfree(pixels);
        if (shadow) kfree(shadow);
        return -ENOMEM;
    }

    unsigned long irq = irq_push();
    spinlock_acquire(&b->fb_lock);

    if (shadow) {
        kfree(fb->pixels);
        kfree(fb->shadow_pixels);
        fb->pixels             = pixels;
        fb->shadow_pixels      = shadow;
        fb->shadow_pixels_size = bytes;
        fb->pitch              = cap_w;
        b->cap_height          = cap_h;
        b->display_pixels      = pixels;
    }

    fb->shadow_initialized = 1;
    fb->width              = width;
    fb->height             = height;

    // every line laid out again for the new width, the cursor goes with the text its in
    fb_text_reflow(fb);
    fb_text_repaint(fb);

    // the display copy is just the shadow again, and all of it counts as changed
    memcpy(fb->pixels, fb->shadow_pixels, (size_t)fb->pitch * height * sizeof(uint32_t));
    fb->dirty_top    = height;
    fb->dirty_bottom = 0;
    fb->grab_top     = 0;
    fb->grab_bottom  = height;

    spinlock_release(&b->fb_lock);
    irq_restore(irq);

    fb_text_spare_free(fb);
    return 0;
}

void tty_task_exit(task_t *task) {
    if (!task) return;

    for (tty_t *tty = g_offscreen; tty; tty = tty->next) {
        if (tty->owner_pid == task->id)
            tty_offscreen_destroy(tty);
    }
}

/// @brief make a tty that draws into a buffer of its own, for a terminal window to show
/// @param width  size of that buffer in pixels
/// @param index_out which /dev/ttyN it is
/// @return 0 on success, negative status code on failure
static int tty_offscreen_create(uint32_t width, uint32_t height, uint32_t *index_out) {
    task_t *task = get_current_task();
    if (!task) return -ESRCH;
    if (width < TTY_OFFSCREEN_MIN || height < TTY_OFFSCREEN_MIN ||
        width > TTY_OFFSCREEN_MAX || height > TTY_OFFSCREEN_MAX)
        return -EINVAL;

    // everything that can fail or take a while happens before a slot is taken.
    // the buffers get some room to grow into, see tty_offscreen_resize
    uint32_t  cap_w  = tty_offscreen_grow(width);
    uint32_t  text_w = tty_offscreen_grow((uint32_t)framebuffer_get_width(0));
    if (text_w < cap_w) text_w = cap_w;
    uint32_t  cap_h  = tty_offscreen_grow(height);
    size_t    bytes  = (size_t)cap_w * cap_h * sizeof(uint32_t);
    uint32_t *pixels = kmalloc(bytes);
    uint32_t *shadow = kmalloc(bytes);
    tty_t    *fresh  = kmalloc(sizeof(tty_t));
    tty_fb_backend_t *fresh_backend = kmalloc(sizeof(tty_fb_backend_t));
    if (!pixels || !shadow || !fresh || !fresh_backend) {
        if (pixels)        kfree(pixels);
        if (shadow)        kfree(shadow);
        if (fresh)         kfree(fresh);
        if (fresh_backend) kfree(fresh_backend);
        return -ENOMEM;
    }
    memset(pixels, 0, bytes);
    memset(shadow, 0, bytes);
    memset(fresh, 0, sizeof(*fresh));
    memset(fresh_backend, 0, sizeof(*fresh_backend));

    unsigned long irq = irq_push();
    spinlock_acquire(&g_offscreen_lock);

    // one thats been used and given back comes first, its /dev node already exists
    uint32_t  index = 1;
    tty_t    *tty   = NULL;
    tty_t   **end   = &g_offscreen;
    for (; *end; end = &(*end)->next, index++) {
        if (!(*end)->owner_pid) {
            tty = *end;
            break;
        }
    }

    bool isnew = !tty;
    if (isnew) {
        tty = fresh;
        tty->backend              = fresh_backend;
        tty->device.ops           = &tty_inode_ops;
        tty->device.internal_data = tty;
        tty->device.type          = INODE_DEVICE;
        tty->device.shared        = 1;
        *end                      = tty;
    }
    tty->owner_pid = task->id;

    spinlock_release(&g_offscreen_lock);
    irq_restore(irq);

    if (!isnew) {
        kfree(fresh);
        kfree(fresh_backend);
    }

    tty_fb_backend_t *b = tty->backend;
    memset(b, 0, sizeof(*b));
    b->fb = (fb_console_t){
        .pixels             = pixels,
        .shadow_pixels      = shadow,
        .shadow_pixels_size = bytes,
        .shadow_initialized = 1,
        .dirty_top   = height,
        .width       = width,
        .height      = height,
        .pitch       = cap_w,
        .font        = psf_get_current_font(),
        .fg          = 0xFFFFFF,
        .bg          = 0x000000,
        .grab_top    = 0,
        .grab_bottom = height,      // all of it, the window starts out showing nothing
    };
    b->display_pixels = pixels;
    b->is_active      = 1;
    b->cap_height     = cap_h;

    // the text is what lets a resize wrap everything again, without it this isnt worth having
    if (!b->fb.font || fb_text_attach(&b->fb, text_w / b->fb.font->width,
                                      TTY_OFFSCREEN_MAX / b->fb.font->height) != 0) {
        tty_offscreen_destroy(tty);
        return -ENOMEM;
    }

    tty->index      = index;
    tty->offscreen  = 1;
    tty->ops        = &tty0_ops;
    tty->in_head    = 0;
    tty->in_tail    = 0;
    tty->line_start = 0;
    tty->flags      = TTY_ECHO | TTY_CANNONICAL;
    tty->termios    = (tty_termios_t){0};
    tty->termios.c_lflag           = TTY_TERM_ECHO | TTY_TERM_ICANON | TTY_TERM_ISIG;
    tty->termios.c_cc[TTY_VINTR]   = 3;
    tty->termios.c_cc[TTY_VERASE]  = 127;
    tty->termios.c_cc[TTY_VMIN]    = 1;
    tty_sync_termios_from_flags(tty);
    spinlock_init(&tty->in_lock);

    if (isnew) {
        char name[16];
        snprintf(name, sizeof(name), "tty%u", index);
        long reg = device_register(&tty->device, name);
        if (reg < 0) {
            // the slot keeps its tty_t, it just never got a /dev node. hand the pixels back
            tty_offscreen_destroy(tty);
            return (int)reg;
        }
    }

    *index_out = index;
    return 0;
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
            uint32_t index = tty->index;

            // a shell works out if its the one being looked at by asking /dev/tty0
            if (tty == &g_tty0 && g_active_offscreen && tty_of_task(get_current_task()) == g_active_offscreen)
                index = g_active_offscreen->index;
            return tty_arg_out(arg, &index, sizeof(index));
        }

        case TTY_IOCTL_GET_ACTIVE_INDEX: {
            uint32_t index = g_active_offscreen ? g_active_offscreen->index : 0;
            return tty_arg_out(arg, &index, sizeof(index));
        }

        // arg is just yes or no, not a pointer. the window that owns the tty says when it has the focus
        case TTY_IOCTL_SET_ACTIVE: {
            task_t *task = get_current_task();
            if (!tty->offscreen || !task || tty->owner_pid != task->id) return -EPERM;

            if (arg)
                g_active_offscreen = tty;
            else if (g_active_offscreen == tty)
                g_active_offscreen = NULL;
            return 0;
        }

        case TTY_IOCTL_RESIZE: {
            task_t *task = get_current_task();
            if (!tty->offscreen || !task || tty->owner_pid != task->id) return -EPERM;

            tty_create_t size;
            if ((r = tty_arg_in(arg, &size, sizeof(size))) < 0) return r;

            uint32_t old_cols, old_rows, cols, rows;
            tty_fill_winsize(tty, &old_cols, &old_rows);
            if ((r = tty_offscreen_resize(tty, size.width, size.height)) < 0) return r;
            tty_fill_winsize(tty, &cols, &rows);

            // only when the text grid itself changed, a few pixels either way is nobodys business
            if (cols != old_cols || rows != old_rows)
                itterate_each_task(tty_send_winch, tty);

            sched_wake_event_waiters();
            return 0;
        }

        case TTY_IOCTL_CREATE: {
            tty_create_t create;
            if ((r = tty_arg_in(arg, &create, sizeof(create))) < 0) return r;
            if ((r = tty_offscreen_create(create.width, create.height, &create.index)) < 0) return r;
            return tty_arg_out(arg, &create, sizeof(create));
        }

        case TTY_IOCTL_DESTROY: {
            task_t *task = get_current_task();
            if (!tty->offscreen || !task || tty->owner_pid != task->id) return -EPERM;
            tty_offscreen_destroy(tty);
            return 0;
        }

        case TTY_IOCTL_GRAB: {
            task_t *task = get_current_task();
            if (!tty->offscreen || !task || tty->owner_pid != task->id) return -EPERM;

            tty_grab_t grab;
            if ((r = tty_arg_in(arg, &grab, sizeof(grab))) < 0) return r;

            unsigned long irq = irq_push();
            spinlock_acquire(&b->fb_lock);

            size_t top    = b->fb.grab_top;
            size_t bottom = b->fb.grab_bottom;
            b->fb.grab_top    = b->fb.height;
            b->fb.grab_bottom = 0;

            spinlock_release(&b->fb_lock);
            irq_restore(irq);

            grab.top    = 0;
            grab.bottom = 0;
            if (top < bottom && b->fb.pixels) {
                // copied with the lock gone because the copy can fault. if something draws
                // halfway through it marks the rows again and the next grab puts it right
                size_t row = b->fb.pitch * sizeof(uint32_t);

                if (!grab.pitch || grab.pitch == b->fb.pitch) {
                    if (copy_to_user(task, (void *)(uintptr_t)(grab.buffer + top * row),
                                     b->fb.pixels + top * b->fb.pitch, (bottom - top) * row) != 0)
                        return -EFAULT;
                } else {
                    // the callers rows are further apart than ours, so its a row at a time
                    if (grab.pitch < b->fb.width) return -EINVAL;

                    size_t out_row = (size_t)grab.pitch * sizeof(uint32_t);
                    for (size_t y = top; y < bottom; y++) {
                        if (copy_to_user(task, (void *)(uintptr_t)(grab.buffer + y * out_row),
                                         b->fb.pixels + y * b->fb.pitch, b->fb.width * sizeof(uint32_t)) != 0)
                            return -EFAULT;
                    }
                }

                grab.top    = (uint32_t)top;
                grab.bottom = (uint32_t)bottom;
            }
            return tty_arg_out(arg, &grab, sizeof(grab));
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

            if (tty->offscreen)
                sched_wake_event_waiters();

            // copied out after the lock is gone, the copy can fault
            return tty_arg_out(arg, &view, sizeof(view));
        }

        default:
            return -ENOTTY;
    }
}

// same rules tty_read uses to decide whether it has anything to hand over
static int tty_poll(INode_t *dev) {
    tty_t *tty = dev->internal_data;
    if (!tty) return POLLERR;

    int ready = POLLOUT;

    tty_fb_backend_t *b = tty->backend;
    if (tty->offscreen && b && b->fb.grab_top < b->fb.grab_bottom)
        ready |= POLLPRI;

    if (!(tty->flags & TTY_CANNONICAL)) {
        if (tty->in_head != tty->in_tail) ready |= POLLIN;
        return ready;
    }

    // canonical only gives out whole lines
    for (size_t i = tty->in_tail; i != tty->in_head; i = (i + 1) % TTY_BUFFER_SZ) {
        if (tty->inbuffer[i] == '\n') { ready |= POLLIN; break; }
    }
    return ready;
}

/// @brief hand the screen to userspace or take it back
// while its on, tty0 only draws into its shadow 
void tty_set_graphics(int on) {
    if (!g_tty0_ready) return;
    tty_fb_backend_t *b = &g_tty0_backend;

    unsigned long irq = irq_push();
    spinlock_acquire(&b->fb_lock);

    b->fb.detached = on ? 1 : 0;
    if (!on) fb_console_render_view(&b->fb);

    spinlock_release(&b->fb_lock);
    irq_restore(irq);
}

static struct tty_ops tty0_ops = {
    .putchar = tty_fb_putchar,
};

struct INodeOps tty_inode_ops = {
    .write = tty_inode_write,
    .read  = tty_read,
    .ioctl = tty_ioctl,
    .poll  = tty_poll,
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

    if (fb.font)
        fb_text_attach(&b->fb, fb.width / fb.font->width, fb.height / fb.font->height);

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