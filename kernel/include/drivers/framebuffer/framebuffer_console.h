#pragma once
#include <stdint.h>
#include <fonts/psf.h>

typedef struct {
    uint32_t *pixels;
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
} fb_console_t;