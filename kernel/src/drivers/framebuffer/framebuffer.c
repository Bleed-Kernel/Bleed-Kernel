#include <boot/bootloader_interface/generic_bootloader.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <devices/type/tty_device.h>
#include <drivers/serial/serial.h>
#include <kernel/bootargs.h>
#include <mm/kalloc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <ansii.h>

void *framebuffer_get_addr(int idx) {
    (void)idx;
    return (void *)g_gbi.framebuffer.address;
}

uint64_t framebuffer_get_pitch(int idx) {
    (void)idx;
    return g_gbi.framebuffer.pitch;
}

uint64_t framebuffer_get_width(int idx) {
    (void)idx;
    return g_gbi.framebuffer.width;
}

uint64_t framebuffer_get_height(int idx) {
    (void)idx;
    return g_gbi.framebuffer.height;
}

uint64_t framebuffer_get_bpp(int idx) {
    (void)idx;
    return g_gbi.framebuffer.bpp;
}

#define FB_MAX_DIMENSION 16384

// parse a decimal boot argument, false if it is missing, malformed or zero
static bool fb_bootarg_u32(const char *key, uint32_t *out) {
    const char *val = bootargs_get(key);
    if (!val || !*val) return false;

    uint64_t n = 0;
    for (; *val >= '0' && *val <= '9'; val++) {
        n = n * 10 + (uint64_t)(*val - '0');
        if (n > UINT32_MAX) return false;
    }

    if (*val || n == 0) {
        serial_printf(LOG_ERROR "%s: not a positive whole number, ignored\n", key);
        return false;
    }

    *out = (uint32_t)n;
    return true;
}

///     fb-width=<pixels>  fb-height=<pixels>  fb-pitch=<bytes per scanline>  fb-bpp=<bits>
/// the result is checked as a whole, if it doesnt describe a sane mode none of it is applied
void framebuffer_apply_bootargs(void) {
    if (!g_gbi.framebuffer.present) return;

    uint32_t width  = g_gbi.framebuffer.width;
    uint32_t height = g_gbi.framebuffer.height;
    uint32_t pitch  = g_gbi.framebuffer.pitch;
    uint32_t bpp    = g_gbi.framebuffer.bpp;

    bool changed = false;
    changed |= fb_bootarg_u32("fb-width",  &width);
    changed |= fb_bootarg_u32("fb-height", &height);
    changed |= fb_bootarg_u32("fb-bpp",    &bpp);

    bool pitch_given = fb_bootarg_u32("fb-pitch", &pitch);
    changed |= pitch_given;

    if (!changed) return;

    if (bpp != 16 && bpp != 24 && bpp != 32) {
        serial_printf(LOG_ERROR "fb: fb-bpp=%u is not 16, 24 or 32, overrides ignored\n", bpp);
        return;
    }

    if (width > FB_MAX_DIMENSION || height > FB_MAX_DIMENSION) {
        serial_printf(LOG_ERROR "fb: %ux%u is out of range, overrides ignored\n", width, height);
        return;
    }

    // a wider mode needs a longer scanline
    uint32_t bytes_per_pixel = bpp / 8;
    uint32_t min_pitch = width * bytes_per_pixel;
    if (!pitch_given && pitch < min_pitch)
        pitch = min_pitch;

    if (pitch < min_pitch || (pitch % bytes_per_pixel)) {
        serial_printf(LOG_ERROR "fb: fb-pitch=%u doesnt fit %u pixels at %ubpp (need >= %u), overrides ignored\n",
                      pitch, width, bpp, min_pitch);
        return;
    }

    serial_printf(LOG_INFO "fb: mode overridden %ux%u pitch %u %ubpp -> %ux%u pitch %u %ubpp\n",
                  g_gbi.framebuffer.width, g_gbi.framebuffer.height,
                  g_gbi.framebuffer.pitch, g_gbi.framebuffer.bpp,
                  width, height, pitch, bpp);

    g_gbi.framebuffer.width  = width;
    g_gbi.framebuffer.height = height;
    g_gbi.framebuffer.pitch  = pitch;
    g_gbi.framebuffer.bpp    = (uint16_t)bpp;
}

static void fb_scrollback_ensure(fb_console_t *fb) {
    if (!fb->font || !fb->pitch) return;

    size_t new_lw = (size_t)fb->font->height * fb->pitch;

    if (fb->scrollback_lines && fb->scrollback_line_words == new_lw) return;

    if (fb->scrollback_lines)
        kfree(fb->scrollback_lines);

    fb->scrollback_line_words = new_lw;
    fb->scrollback_lines = kmalloc(fb->scrollback_line_words * FB_SCROLLBACK_LINES * sizeof(uint32_t));
    fb->scrollback_head  = 0;
    fb->scrollback_count = 0;
    fb->scrollback_view  = 0;
}

void fb_scrollback_reset(fb_console_t *fb) {
    if (!fb) return;
    fb->scrollback_head  = 0;
    fb->scrollback_count = 0;
    fb->scrollback_view  = 0;
}

void fb_scrollback_capture(fb_console_t *fb, const uint32_t *line_src) {
    if (!fb || !line_src) return;

    fb_scrollback_ensure(fb);
    if (!fb->scrollback_lines) return;

    size_t lw = fb->scrollback_line_words;
    uint32_t *dst = fb->scrollback_lines + (fb->scrollback_head * lw);
    memcpy(dst, line_src, lw * sizeof(uint32_t));

    fb->scrollback_head = (fb->scrollback_head + 1) % FB_SCROLLBACK_LINES;
    if (fb->scrollback_count < FB_SCROLLBACK_LINES) fb->scrollback_count++;
}

static void fb_console_clear_row_block(fb_console_t *fb, size_t row, uint32_t colour) {
    uint32_t *dst = fb->pixels + row * fb->scrollback_line_words;
    for (size_t i = 0; i < fb->scrollback_line_words; i++)
        dst[i] = colour;
}

void fb_console_render_view(fb_console_t *fb) {
    if (!fb || !fb->pixels || !fb->font) return;

    size_t visible_rows = fb->height / fb->font->height;
    size_t lw = fb->scrollback_line_words;

    if (fb->scrollback_view == 0 || lw == 0) {
        if (fb->shadow_pixels)
            framebuffer_blit(fb->shadow_pixels, fb->pixels, fb->width, fb->height, fb->pitch);
        return;
    }

    size_t cap  = FB_SCROLLBACK_LINES;
    long   base = (long)fb->scrollback_count - (long)fb->scrollback_view;

    for (size_t r = 0; r < visible_rows; r++) {
        long logical = base + (long)r;
        const uint32_t *src = NULL;

        if (logical < 0) {
            fb_console_clear_row_block(fb, r, fb->bg);
            continue;
        } else if ((size_t)logical < fb->scrollback_count) {
            size_t slot = (fb->scrollback_head + cap - fb->scrollback_count + (size_t)logical) % cap;
            src = fb->scrollback_lines + slot * lw;
        } else if (fb->shadow_pixels) {
            size_t shadow_row = (size_t)logical - fb->scrollback_count;
            src = fb->shadow_pixels + shadow_row * lw;
        }

        if (!src) {
            fb_console_clear_row_block(fb, r, fb->bg);
            continue;
        }

        memcpy(fb->pixels + r * lw, src, lw * sizeof(uint32_t));
    }
}

void fb_console_scroll(fb_console_t *fb, int lines) {
    if (!fb || lines == 0 || !fb->font) return;

    fb_scrollback_ensure(fb);

    long new_view = (long)fb->scrollback_view - lines;
    if (new_view < 0) new_view = 0;
    if ((size_t)new_view > fb->scrollback_count) new_view = (long)fb->scrollback_count;

    fb->scrollback_view = (size_t)new_view;
    fb_console_render_view(fb);
}

void framebuffer_clear(uint32_t *pixels, uint64_t width, uint64_t height, uint64_t pitch, uint32_t colour) {
    if (!pixels) return;
    (void)width;

    uint64_t total_words = height * pitch;
    for (uint64_t i = 0; i < total_words; i++)
        pixels[i] = colour;
}