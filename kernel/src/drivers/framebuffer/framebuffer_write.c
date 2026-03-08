#include <stdint.h>
#include <string.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <mm/spinlock.h>
#include <fonts/utf-8.h>
#include <mm/kalloc.h>
#include <panic.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

void framebuffer_init_buffer(fb_console_t *fb);

static uint32_t *framebuffer_get_shadow_buffer(fb_console_t *fb) {
    if (!fb)
        return NULL;

    size_t size = fb->pitch * fb->height * sizeof(uint32_t);
    if (!fb->shadow_pixels || fb->shadow_pixels_size != size) {
        fb->shadow_pixels = kmalloc(size);
        fb->shadow_pixels_size = size;
        fb->shadow_initialized = 0;
    }

    return fb->shadow_pixels;
}

uint32_t* framebuffer_get_buffer(void) {
    return NULL;
}

void framebuffer_load_shadow(fb_console_t *fb, const uint32_t *src) {
    if (!fb || !src)
        return;

    uint32_t *fb_buffer = framebuffer_get_shadow_buffer(fb);
    if (!fb_buffer)
        return;

    size_t total_bytes = (size_t)fb->pitch * (size_t)fb->height * sizeof(uint32_t);
    memcpy(fb_buffer, src, total_bytes);
    fb->shadow_initialized = 1;
    fb->dirty_top = fb->height;
    fb->dirty_bottom = 0;
}

void framebuffer_flush(fb_console_t *fb) {
    if (!fb)
        return;

    uint32_t *fb_buffer = framebuffer_get_shadow_buffer(fb);
    if (!fb_buffer) return;

    if (fb->dirty_top < fb->dirty_bottom) {
        uint32_t* src = fb_buffer + fb->dirty_top * fb->pitch;
        uint32_t* dst = fb->pixels + fb->dirty_top * fb->pitch;
        framebuffer_blit(src, dst, fb->width, fb->dirty_bottom - fb->dirty_top, fb->pitch);
    }

    fb->dirty_top = fb->height;
    fb->dirty_bottom = 0;
}

void framebuffer_init_buffer(fb_console_t *fb) {
    uint32_t *fb_buffer = framebuffer_get_shadow_buffer(fb);
    if (!fb_buffer)
        return;

    size_t total_pixels = (size_t)fb->pitch * (size_t)fb->height;
    for (size_t i = 0; i < total_pixels; i++) {
        fb_buffer[i] = fb->bg;
    }
    fb->shadow_initialized = 1;
    fb->dirty_top = 0;
    fb->dirty_bottom = fb->height;
    framebuffer_flush(fb);
}

void framebuffer_mark_dirty(fb_console_t *fb, size_t y, size_t height) {
    if (y < fb->dirty_top) fb->dirty_top = y;
    if (y + height > fb->dirty_bottom) fb->dirty_bottom = y + height;
}

static inline void framebuffer_clear_row(uint32_t* buffer, size_t y, size_t pitch, uint32_t color) {
    if (!buffer) return;
    uint32_t* row = buffer + y * pitch;
    for (size_t i = 0; i < pitch; i++)
        row[i] = color;
}

static void framebuffer_render_glyph_index(fb_console_t *fb, size_t row, size_t col, uint16_t idx, uint32_t fg, uint32_t bg) {
    uint32_t *fb_buffer = framebuffer_get_shadow_buffer(fb);
    if (!fb_buffer)
        return;

    const uint8_t *glyph = psf_get_glyph_font(fb->font, idx);
    if (!glyph) return;

    size_t px = col * fb->font->width;
    size_t py = row * fb->font->height;
    size_t glyph_w = fb->font->width;
    size_t bytes_per_row = fb->font->bytes_per_row;

    uint32_t expanded_row[64];

    for (uint32_t r = 0; r < fb->font->height; r++) {
        uint32_t *dst = fb_buffer + (py + r) * fb->pitch + px;
        size_t out_idx = 0;

        for (unsigned int byte = 0; byte < bytes_per_row; byte++) {
            uint8_t bits = glyph[r * bytes_per_row + byte];
            for (int bit = 0; bit < 8 && out_idx < glyph_w; bit++) {
                uint32_t mask = -((bits & (0x80 >> bit)) != 0);
                expanded_row[out_idx++] = (mask & fg) | (~mask & bg);
            }
        }

        memcpy(dst, expanded_row, out_idx * sizeof(uint32_t));
        framebuffer_mark_dirty(fb, py + r, 1);
    }
}

static void framebuffer_scroll_buffer(fb_console_t *fb) {
    size_t row_px = fb->font->height;
    uint32_t *fb_buffer = framebuffer_get_shadow_buffer(fb);
    if (!fb_buffer) return;

    memmove(fb_buffer,
            fb_buffer + row_px * fb->pitch,
            (fb->height - row_px) * fb->pitch * sizeof(uint32_t));

    for (size_t y = fb->height - row_px; y < fb->height; y++)
        framebuffer_clear_row(fb_buffer, y, fb->pitch, fb->bg);

    fb->cursor_y = (fb->height / fb->font->height) - 1;

    fb->dirty_top = 0;
    fb->dirty_bottom = fb->height;

    framebuffer_flush(fb);
}

static inline void framebuffer_draw_glyph_row_mem(fb_console_t *fb, size_t x, size_t y,
                                                  const uint8_t *glyph_row, uint32_t fg, uint32_t bg) {
    uint32_t *fb_buffer = framebuffer_get_shadow_buffer(fb);
    if (y >= fb->height || !fb_buffer) return;
    framebuffer_mark_dirty(fb, y, 1);

    uint32_t* dst = fb_buffer + y * fb->pitch + x;
    size_t w = fb->font->width;

    for (unsigned int byte = 0; byte < fb->font->bytes_per_row; byte++) {
        uint8_t bits = glyph_row[byte];
        for (int bit = 0; bit < 8 && (byte*8+bit) < w; bit++) {
            if (x + byte*8 + bit < fb->width)
                dst[byte*8+bit] = (bits & (0x80 >> bit)) ? fg : bg;
        }
    }
}

void framebuffer_put_char(fb_console_t *fb, uint32_t c) {
    if (!framebuffer_get_shadow_buffer(fb))
        return;
    if (!fb->shadow_initialized)
        framebuffer_init_buffer(fb);

    size_t max_cols = fb->width / fb->font->width;
    size_t max_rows = fb->height / fb->font->height;

    switch (c) {
    case '\n': fb->cursor_x = 0; fb->cursor_y++; break;
    case '\r': fb->cursor_x = 0; break;
    case '\b': if (fb->cursor_x) fb->cursor_x--; break;
    case '\t': fb->cursor_x = (fb->cursor_x + 8) & ~7; break;
    default:
        uint16_t idx = psf_lookup_glyph(fb->font, c);
        framebuffer_render_glyph_index(fb, fb->cursor_y, fb->cursor_x, idx, fb->fg, fb->bg);
        fb->cursor_x++;
        break;
    }

    if (fb->cursor_x >= max_cols) {
        fb->cursor_x = 0;
        fb->cursor_y++;
    }

    if (fb->cursor_y >= max_rows)
        framebuffer_scroll_buffer(fb);
    else
        framebuffer_flush(fb);
}

void framebuffer_write_string(fb_console_t *fb, ansii_state_t *ansi, const char *str, spinlock_t *lock) {
    const uint8_t *p = (const uint8_t *)str;
    while (*p) {
        uint32_t cp;
        size_t len = utf8_decode(p, &cp);
        if (!len) {
            p++;
            continue;
        }
        framebuffer_ansi_char(fb, lock, ansi, cp);
        p += len;
    }
}
