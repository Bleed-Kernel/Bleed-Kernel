#include <stdint.h>
#include <string.h>
#include <drivers/framebuffer/framebuffer.h>
#include <console/console.h>
#include <drivers/framebuffer/framebuffer_console.h>

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
        }
        st->esc = 0;
        return;
    }

    if (st->csi) {
        if (c == 'J') {
            fb_clear(fb);
            st->csi = 0;
            return;
        }

        if (c >= '0' && c <= '9') {
            if (st->substate == 0)
                st->params[st->param_count] = st->params[st->param_count] * 10 + (c - '0');
            else
                st->subparams[st->substate - 1] = st->subparams[st->substate - 1] * 10 + (c - '0');
            return;
        }

        if (c == ';') {
            if (st->substate) st->substate++;
            else st->param_count++;
            return;
        }

        if (c == 'm') {
            int i = 0;
            while (i <= st->param_count) {
                int p = st->params[i];
                if (p == 0) {
                    fb->fg = 0xFFFFFFFF;
                    fb->bg = 0x00000000;
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
    }

    if (c == 0x1B) {
        st->esc = 1;
        return;
    }

    spinlock_acquire(framebuffer_lock);
    framebuffer_put_char(fb, c);
    spinlock_release(framebuffer_lock);
}