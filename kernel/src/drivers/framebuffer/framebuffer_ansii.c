#include <stdint.h>
#include <string.h>
#include <drivers/framebuffer/framebuffer.h>
#include <console/console.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <mm/spinlock.h>

static size_t ansi_clamp(long v, size_t max) {
    if (v < 0) return 0;
    return (size_t)v < max ? (size_t)v : max - 1;
}

// cursor moves and erases, the same meanings linux and every terminal give them
static void framebuffer_ansi_csi(fb_console_t *fb, ansii_state_t *st, uint32_t c) {
    size_t cols = fb->width / fb->font->width;
    size_t rows = fb->height / fb->font->height;
    size_t x    = fb->cursor_x;
    size_t y    = fb->cursor_y;
    int    p0   = st->params[0];
    long   n    = p0 ? p0 : 1;

    switch (c) {
        case 'A': fb->cursor_y = ansi_clamp((long)y - n, rows); break;
        case 'B': fb->cursor_y = ansi_clamp((long)y + n, rows); break;
        case 'C': fb->cursor_x = ansi_clamp((long)x + n, cols); break;
        case 'D': fb->cursor_x = ansi_clamp((long)x - n, cols); break;
        case 'G': fb->cursor_x = ansi_clamp(n - 1, cols); break;
        case 'H':
        case 'f':
            fb->cursor_y = ansi_clamp(n - 1, rows);
            fb->cursor_x = ansi_clamp(st->param_count >= 1 && st->params[1] ? st->params[1] - 1 : 0, cols);
            break;

        case 'h':
        case 'l':
            if (st->priv && p0 == 7)
                fb->no_wrap = (c == 'l');
            break;
        case 'K':
            if (p0 == 0) framebuffer_erase(fb, y, x, cols);
            if (p0 == 1) framebuffer_erase(fb, y, 0, x + 1);
            if (p0 == 2) framebuffer_erase(fb, y, 0, cols);
            break;

        case 'J':
            // 2 and 3 are the whole screen, which also drops the history. the cursor stays put
            if (p0 >= 2) {
                fb_clear(fb);
                fb->cursor_x = x;
                fb->cursor_y = y;
                break;
            }
            if (p0 == 0) {
                framebuffer_erase(fb, y, x, cols);
                for (size_t r = y + 1; r < rows; r++)
                    framebuffer_erase(fb, r, 0, cols);
            } else {
                for (size_t r = 0; r < y; r++)
                    framebuffer_erase(fb, r, 0, cols);
                framebuffer_erase(fb, y, 0, x + 1);
            }
            break;
    }
    framebuffer_flush(fb);
}

static const uint32_t ansi_palette[16] = {
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
    0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

static uint32_t ansi_256(int n) {
    if (n < 16)
        return 0xFF000000 | ansi_palette[n < 0 ? 0 : n];
    if (n < 232) {
        static const uint8_t level[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return 0xFF000000 | (level[n / 36] << 16) | (level[(n / 6) % 6] << 8) | level[n % 6];
    }
    uint8_t grey = (uint8_t)(8 + (n > 255 ? 23 : n - 232) * 10);
    return 0xFF000000 | (grey << 16) | (grey << 8) | grey;
}

/// @brief evaluate c and track its ansii state
/// @param c target
void framebuffer_ansi_char(fb_console_t *fb, spinlock_t *framebuffer_lock, ansii_state_t *st, uint32_t c) {
    if (!fb || !st) return;

    if (st->esc) {
        if (c == '[') {
            st->csi = 1;
            st->param_count = 0;
            memset(st->params, 0, sizeof(st->params));
            st->substate = 0;
            st->priv = 0;
        }
        st->esc = 0;
        return;
    }

    if (st->csi) {
        if (c >= '0' && c <= '9') {
            if (st->substate == 0)
                st->params[st->param_count] = st->params[st->param_count] * 10 + (c - '0');
            else
                st->subparams[st->substate - 1] = st->subparams[st->substate - 1] * 10 + (c - '0');
            return;
        }

        if (c == ';') {
            if (st->substate) st->substate++;
            else if (st->param_count < 15) st->param_count++;
            return;
        }

        if (c == 'm') {
            int i = 0;
            while (i <= st->param_count) {
                int p = st->params[i];
                if (p == 0) {
                    fb->fg = 0xFFFFFFFF;
                    fb->bg = 0x00000000;
                } else if (p >= 30 && p <= 37) {
                    fb->fg = 0xFF000000 | ansi_palette[p - 30];
                } else if (p >= 90 && p <= 97) {
                    fb->fg = 0xFF000000 | ansi_palette[p - 90 + 8];
                } else if (p == 39) {
                    fb->fg = 0xFFFFFFFF;
                } else if (p >= 40 && p <= 47) {
                    fb->bg = 0xFF000000 | ansi_palette[p - 40];
                } else if (p >= 100 && p <= 107) {
                    fb->bg = 0xFF000000 | ansi_palette[p - 100 + 8];
                } else if (p == 49) {
                    fb->bg = 0x00000000;
                } else if ((p == 38 || p == 48) && (i + 2 <= st->param_count) && st->params[i + 1] == 5) {
                    if (p == 38)
                        fb->fg = ansi_256(st->params[i + 2]);
                    else
                        fb->bg = ansi_256(st->params[i + 2]);
                    i += 2;
                } else if (p == 38 && (i + 4 <= st->param_count) && st->params[i + 1] == 2) {
                    uint8_t r = st->params[i+2];
                    uint8_t g = st->params[i+3];
                    uint8_t b = st->params[i+4];
                    fb->fg = 0xFF000000 | (r << 16) | (g << 8) | b;
                    i += 4;
                } else if (p == 48 && (i + 4 <= st->param_count) && st->params[i + 1] == 2) {
                    uint8_t r = st->params[i+2];
                    uint8_t g = st->params[i+3];
                    uint8_t b = st->params[i+4];
                    fb->bg = 0xFF000000 | (r << 16) | (g << 8) | b;
                    i += 4;
                }
                i++;
            }
            st->csi = 0;
            return;
        }

        if (c == '?') {
            st->priv = 1;
            return;
        }

        // anything else up to the final byte is ignored
        if (c < 0x40 || c > 0x7E)
            return;

        st->csi = 0;
        if (!fb->font) return;

        unsigned long irq = irq_push();
        spinlock_acquire(framebuffer_lock);
        framebuffer_ansi_csi(fb, st, c);
        spinlock_release(framebuffer_lock);
        irq_restore(irq);
        return;
    }

    if (c == 0x1B) {
        st->esc = 1;
        return;
    }

    unsigned long irq = irq_push();
    spinlock_acquire(framebuffer_lock);
    framebuffer_put_char(fb, c);
    spinlock_release(framebuffer_lock);
    irq_restore(irq);
}