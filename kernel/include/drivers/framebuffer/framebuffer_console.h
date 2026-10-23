#pragma once
#include <stdint.h>
#include <fonts/psf.h>

#define FB_SCROLLBACK_LINES 500

// one character on screen and the colours it was drawn in, cp 0 means nothing was ever put there
typedef struct {
    uint32_t cp;
    uint32_t fg;
    uint32_t bg;
} fb_cell_t;

// What an offscreen tty has on it as text instead of only as pixels, so that changing its
// width can wrap every line again. Rows are kept oldest first
typedef struct fb_text {
    size_t     cols_cap;    // cells in a stored row, the most columns it can ever have
    size_t     rows_cap;    // rows of storage, history and the biggest screen together
    size_t     cols;        // how its laid out right now
    size_t     rows;
    size_t     history;     // rows above the top of the screen
    size_t     start;       // ring position of the oldest row

    fb_cell_t *cells;
    uint16_t  *used;        // cells written in each row
    uint8_t   *wrapped;

    // somewhere to build the new layout during a reflow, swapped with the above afterwards.
    // NULL outside of one
    fb_cell_t *spare_cells;
    uint16_t  *spare_used;
    uint8_t   *spare_wrapped;
} fb_text_t;

typedef struct {
    uint32_t *pixels;
    uint32_t *shadow_pixels;
    size_t shadow_pixels_size;
    uint8_t shadow_initialized;
    uint8_t detached;           // only the shadow gets drawn to if something else owns the screen
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    psf_font_t *font;

    uint32_t fg;
    uint32_t bg;

    size_t cursor_x;
    size_t cursor_y;
    uint8_t no_wrap;

    size_t dirty_top;
    size_t dirty_bottom;

    size_t grab_top;
    size_t grab_bottom;

    uint32_t *scrollback_lines;
    size_t    scrollback_line_words;
    size_t    scrollback_head;
    size_t    scrollback_count;
    size_t    scrollback_view;

    fb_text_t *text;        // NULL for a console that never changes size
} fb_console_t;