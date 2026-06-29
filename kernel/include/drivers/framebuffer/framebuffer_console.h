#pragma once
#include <stdint.h>
#include <fonts/psf.h>

#define FB_SCROLLBACK_LINES 500

typedef struct {
    uint32_t *pixels;
    uint32_t *shadow_pixels;
    size_t shadow_pixels_size;
    uint8_t shadow_initialized;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    psf_font_t *font;

    uint32_t fg;
    uint32_t bg;

    size_t cursor_x;
    size_t cursor_y;

    size_t dirty_top;
    size_t dirty_bottom;

    uint32_t *scrollback_lines;
    size_t    scrollback_line_words;
    size_t    scrollback_head;
    size_t    scrollback_count;
    size_t    scrollback_view;
} fb_console_t;