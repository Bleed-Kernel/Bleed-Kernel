#include <stdint.h>
#include <string.h>
#include <vendor/limine_bootloader/limine.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <mm/spinlock.h>
#include <mm/kalloc.h>
#include <panic.h>

#include "blit.h"

extern volatile struct limine_framebuffer_request framebuffer_request;

static uint32_t *fb_buffer = NULL;
static size_t fb_buffer_size = 0;

uint32_t* framebuffer_get_buffer(void) {
    return fb_buffer;
}

void framebuffer_flush(fb_console_t *fb) {
    if (!fb_buffer) return;

    if (fb->dirty_top < fb->dirty_bottom) {
        uint32_t* src = fb_buffer + fb->dirty_top * fb->pitch;
        uint32_t* dst = fb->pixels + fb->dirty_top * fb->pitch;
        framebuffer_blit(src, dst, fb->width, fb->dirty_bottom - fb->dirty_top);
    }

    fb->dirty_top = fb->height;
    fb->dirty_bottom = 0;
}

void framebuffer_init_buffer(fb_console_t *fb) {
    size_t size = fb->pitch * fb->height * sizeof(uint32_t);
    if (size != fb_buffer_size) {
        fb_buffer = kmalloc(size);
        fb_buffer_size = size;
    }
    memset(fb_buffer, 0, fb_buffer_size);
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

static void framebuffer_scroll_buffer(fb_console_t *fb) {
    size_t row_px = fb->font->height;
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

static void framebuffer_render_char_mem(fb_console_t *fb, size_t row, size_t col, char c, uint32_t fg, uint32_t bg) {
    if (!c) return;
    const uint8_t *glyph = psf_get_glyph_font(fb->font, (uint16_t)c);
    if (!glyph) return;

    size_t px = col * fb->font->width;
    size_t py = row * fb->font->height;

    for (uint32_t r = 0; r < fb->font->height; r++)
        framebuffer_draw_glyph_row_mem(fb, px, py + r, &glyph[r * fb->font->bytes_per_row], fg, bg);
}

void framebuffer_put_char(fb_console_t *fb, char c) {
    if (!fb_buffer) framebuffer_init_buffer(fb);

    size_t max_cols = fb->width / fb->font->width;
    size_t max_rows = fb->height / fb->font->height;

    switch (c) {
    case '\n': fb->cursor_x = 0; fb->cursor_y++; break;
    case '\r': fb->cursor_x = 0; break;
    case '\b': if (fb->cursor_x) fb->cursor_x--; break;
    case '\t': fb->cursor_x = (fb->cursor_x + 8) & ~7; break;
    default:
        if ((uint8_t)c < 0x20) return;
        framebuffer_render_char_mem(fb, fb->cursor_y, fb->cursor_x, c, fb->fg, fb->bg);
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

void framebuffer_write_string(fb_console_t *fb, ansii_state_t *ansi, const char *str, spinlock_t *framebuffer_lock) {
    while (*str)
        framebuffer_ansi_char(fb, framebuffer_lock, ansi, *str++);
}