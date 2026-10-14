#include <stdint.h>
#include <string.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <fonts/psf.h>
#include <mm/kalloc.h>

static inline size_t text_ring(const fb_text_t *t, size_t i) {
    return (t->start + i) % t->rows_cap;
}

// i counts from the oldest row of history, the screen starts at t->history
static inline fb_cell_t *text_row(const fb_text_t *t, size_t i) {
    return t->cells + text_ring(t, i) * t->cols_cap;
}

static void text_row_reset(fb_text_t *t, size_t i) {
    size_t r = text_ring(t, i);
    t->used[r]    = 0;
    t->wrapped[r] = 0;
}

int fb_text_attach(fb_console_t *fb, size_t max_cols, size_t max_rows) {
    if (!fb || !fb->font || !max_cols || !max_rows) return -1;

    fb_text_t *t = kmalloc(sizeof(fb_text_t));
    if (!t) return -1;
    memset(t, 0, sizeof(*t));

    t->cols_cap = max_cols;
    t->rows_cap = FB_SCROLLBACK_LINES + max_rows;

    t->cells   = kmalloc(t->rows_cap * t->cols_cap * sizeof(fb_cell_t));
    t->used    = kmalloc(t->rows_cap * sizeof(uint16_t));
    t->wrapped = kmalloc(t->rows_cap);

    fb->text = t;
    if (!t->cells || !t->used || !t->wrapped) {
        fb_text_detach(fb);
        return -1;
    }

    memset(t->used, 0, t->rows_cap * sizeof(uint16_t));
    memset(t->wrapped, 0, t->rows_cap);

    t->cols = fb->width / fb->font->width;
    t->rows = fb->height / fb->font->height;
    if (t->cols > t->cols_cap) t->cols = t->cols_cap;
    if (t->rows > max_rows)    t->rows = max_rows;
    return 0;
}

int fb_text_spare_alloc(fb_console_t *fb) {
    if (!fb || !fb->text) return -1;
    fb_text_t *t = fb->text;
    if (t->spare_cells) return 0;

    t->spare_cells   = kmalloc(t->rows_cap * t->cols_cap * sizeof(fb_cell_t));
    t->spare_used    = kmalloc(t->rows_cap * sizeof(uint16_t));
    t->spare_wrapped = kmalloc(t->rows_cap);

    if (!t->spare_cells || !t->spare_used || !t->spare_wrapped) {
        fb_text_spare_free(fb);
        return -1;
    }
    return 0;
}

void fb_text_spare_free(fb_console_t *fb) {
    if (!fb || !fb->text) return;
    fb_text_t *t = fb->text;

    if (t->spare_cells)   kfree(t->spare_cells);
    if (t->spare_used)    kfree(t->spare_used);
    if (t->spare_wrapped) kfree(t->spare_wrapped);
    t->spare_cells   = NULL;
    t->spare_used    = NULL;
    t->spare_wrapped = NULL;
}

void fb_text_detach(fb_console_t *fb) {
    if (!fb || !fb->text) return;
    fb_text_t *t = fb->text;

    fb_text_spare_free(fb);
    if (t->cells)   kfree(t->cells);
    if (t->used)    kfree(t->used);
    if (t->wrapped) kfree(t->wrapped);
    kfree(t);
    fb->text = NULL;
}

void fb_text_put(fb_console_t *fb, size_t row, size_t col, uint32_t cp, uint32_t fg, uint32_t bg) {
    fb_text_t *t = fb->text;
    if (!t || row >= t->rows || col >= t->cols_cap) return;

    size_t     r     = text_ring(t, t->history + row);
    fb_cell_t *cells = t->cells + r * t->cols_cap;

    // anything skipped over on the way here was never drawn, dont leave old cells showing through
    for (size_t i = t->used[r]; i < col; i++)
        cells[i] = (fb_cell_t){0};

    cells[col] = (fb_cell_t){ .cp = cp, .fg = fg, .bg = bg };
    if (col + 1 > t->used[r])
        t->used[r] = (uint16_t)(col + 1);
}

void fb_text_wrap(fb_console_t *fb, size_t row, int wrapped) {
    fb_text_t *t = fb->text;
    if (!t || row >= t->rows) return;
    t->wrapped[text_ring(t, t->history + row)] = wrapped ? 1 : 0;
}

void fb_text_scroll(fb_console_t *fb) {
    fb_text_t *t = fb->text;
    if (!t) return;

    // the top row of the screen becomes history, once thats full the oldest row falls off
    if (t->history < FB_SCROLLBACK_LINES)
        t->history++;
    else
        t->start = (t->start + 1) % t->rows_cap;

    text_row_reset(t, t->history + t->rows - 1);
    fb->scrollback_count = t->history;
}

void fb_text_clear(fb_console_t *fb) {
    fb_text_t *t = fb->text;
    if (!t) return;

    t->history = 0;
    t->start   = 0;
    for (size_t i = 0; i < t->rows; i++)
        text_row_reset(t, i);
    fb->scrollback_count = 0;
}

// how much of a row counts as text, blanks hanging off the end of it dont
static size_t text_row_length(const fb_text_t *t, size_t i) {
    size_t           r     = text_ring(t, i);
    const fb_cell_t *cells = t->cells + r * t->cols_cap;
    size_t           len   = t->used[r];

    while (len > 0 && (cells[len - 1].cp == 0 || cells[len - 1].cp == ' '))
        len--;
    return len;
}

void fb_text_reflow(fb_console_t *fb) {
    fb_text_t *t = fb->text;
    if (!t || !fb->font || !t->spare_cells) return;

    size_t new_cols = fb->width / fb->font->width;
    size_t new_rows = fb->height / fb->font->height;
    if (new_cols > t->cols_cap) new_cols = t->cols_cap;
    if (new_rows > t->rows_cap - FB_SCROLLBACK_LINES) new_rows = t->rows_cap - FB_SCROLLBACK_LINES;
    if (!new_cols) new_cols = 1;
    if (!new_rows) new_rows = 1;

    // nothing below the cursor or the last thing written is worth carrying over
    size_t last = fb->cursor_y < t->rows ? fb->cursor_y : t->rows - 1;
    for (size_t r = t->rows; r-- > last + 1; ) {
        if (text_row_length(t, t->history + r)) {
            last = r;
            break;
        }
    }

    size_t in_count   = t->history + last + 1;
    size_t cursor_in  = t->history + (fb->cursor_y < t->rows ? fb->cursor_y : t->rows - 1);
    size_t out_max    = FB_SCROLLBACK_LINES + new_rows;
    size_t out_start  = 0;      // ring position of the oldest row written so far
    size_t out_count  = 0;
    size_t out_total  = 0;      // rows ever written, dropped ones included
    size_t cursor_out = 0;      // counted the same way as out_total
    size_t cursor_col = 0;

    size_t i = 0;
    while (i < in_count) {
        // one line as it was typed or printed: this row and every row it wrapped on into
        size_t first = i;
        size_t end   = i;
        while (end + 1 < in_count && t->wrapped[text_ring(t, end)])
            end++;

        // how long it is, and where along it the cursor sits if its in here
        size_t length = 0;
        size_t offset = (size_t)-1;
        for (size_t r = first; r <= end; r++) {
            size_t row_len = (r == end) ? text_row_length(t, r) : t->used[text_ring(t, r)];
            if (r == cursor_in) {
                offset = length + fb->cursor_x;
                if (fb->cursor_x > row_len) row_len = fb->cursor_x;
            }
            length += row_len;
        }

        // enough rows for the text, and one more if the cursor sits just past a full last row
        size_t need = length ? (length + new_cols - 1) / new_cols : 1;
        if (offset != (size_t)-1 && offset / new_cols + 1 > need)
            need = offset / new_cols + 1;

        if (offset != (size_t)-1) {
            cursor_out = out_total + offset / new_cols;
            cursor_col = offset % new_cols;
        }

        // copy it across a new rows worth at a time
        size_t src_row = first;
        size_t src_col = 0;
        for (size_t n = 0; n < need; n++) {
            if (out_count == out_max) {
                out_start = (out_start + 1) % t->rows_cap;
                out_count--;
            }

            size_t     o   = (out_start + out_count) % t->rows_cap;
            fb_cell_t *dst = t->spare_cells + o * t->cols_cap;
            size_t     got = 0;

            while (got < new_cols && src_row <= end) {
                size_t r       = text_ring(t, src_row);
                size_t row_len = (src_row == end) ? text_row_length(t, src_row) : t->used[r];
                if (src_col >= row_len) {
                    src_row++;
                    src_col = 0;
                    continue;
                }
                dst[got++] = t->cells[r * t->cols_cap + src_col++];
            }

            t->spare_used[o]    = (uint16_t)got;
            t->spare_wrapped[o] = (n + 1 < need) ? 1 : 0;
            out_count++;
            out_total++;
        }

        i = end + 1;
    }

    // swap the new layout in
    fb_cell_t *cells   = t->cells;
    uint16_t  *used    = t->used;
    uint8_t   *wrapped = t->wrapped;
    t->cells         = t->spare_cells;
    t->used          = t->spare_used;
    t->wrapped       = t->spare_wrapped;
    t->spare_cells   = cells;
    t->spare_used    = used;
    t->spare_wrapped = wrapped;

    t->start = out_start;
    t->cols  = new_cols;
    t->rows  = new_rows;

    // the screen is the last rows there are, anything before that is history
    t->history = out_count > new_rows ? out_count - new_rows : 0;
    for (size_t r = out_count; r < t->history + new_rows; r++)
        text_row_reset(t, r);

    size_t dropped    = out_total - out_count;
    size_t cursor_row = cursor_out >= dropped ? cursor_out - dropped : 0;
    fb->cursor_y = cursor_row >= t->history ? cursor_row - t->history : 0;
    fb->cursor_x = cursor_col;
    if (fb->cursor_y >= new_rows) fb->cursor_y = new_rows - 1;

    fb->scrollback_count = t->history;
    fb->scrollback_view  = 0;
}

// one row of text into dst, which is laid out like the screen. i is the row counted from the
// oldest history, past the end just gives a blank row
static void text_draw_row(fb_console_t *fb, uint32_t *dst, size_t screen_row, size_t i) {
    fb_text_t  *t      = fb->text;
    psf_font_t *font   = fb->font;
    size_t      glyph_w = font->width;
    size_t      glyph_h = font->height;
    uint32_t   *band   = dst + screen_row * glyph_h * fb->pitch;

    for (size_t y = 0; y < glyph_h; y++) {
        uint32_t *line = band + y * fb->pitch;
        for (size_t x = 0; x < fb->pitch; x++)
            line[x] = fb->bg;
    }

    if (i >= t->history + t->rows) return;

    size_t           r     = text_ring(t, i);
    const fb_cell_t *cells = t->cells + r * t->cols_cap;
    size_t           count = t->used[r] < t->cols ? t->used[r] : t->cols;

    for (size_t col = 0; col < count; col++) {
        if (!cells[col].cp) continue;

        const uint8_t *glyph = psf_get_glyph_font(font, psf_lookup_glyph(font, cells[col].cp));
        if (!glyph) continue;

        for (size_t y = 0; y < glyph_h; y++) {
            uint32_t *out = band + y * fb->pitch + col * glyph_w;
            for (size_t x = 0; x < glyph_w; x++) {
                uint8_t bits = glyph[y * font->bytes_per_row + x / 8];
                out[x] = (bits & (0x80 >> (x % 8))) ? cells[col].fg : cells[col].bg;
            }
        }
    }
}

void fb_text_repaint(fb_console_t *fb) {
    fb_text_t *t = fb->text;
    if (!t || !fb->shadow_pixels || !fb->font) return;

    for (size_t row = 0; row < t->rows; row++)
        text_draw_row(fb, fb->shadow_pixels, row, t->history + row);

    // the strip under the last whole row, a window is rarely an exact number of rows tall
    for (size_t y = t->rows * fb->font->height; y < fb->height; y++) {
        uint32_t *line = fb->shadow_pixels + y * fb->pitch;
        for (size_t x = 0; x < fb->pitch; x++)
            line[x] = fb->bg;
    }
}

void fb_text_copy_row(const fb_console_t *fb, size_t screen_row, fb_cell_t *out, size_t cols) {
    const fb_text_t *t = fb->text;
    memset(out, 0, cols * sizeof(fb_cell_t));
    if (!t || screen_row >= t->rows) return;

    size_t view = fb->scrollback_view < t->history ? fb->scrollback_view : t->history;
    size_t i    = t->history - view + screen_row;
    if (i >= t->history + t->rows) return;

    size_t r     = text_ring(t, i);
    size_t count = t->used[r] < t->cols ? t->used[r] : t->cols;
    if (count > cols) count = cols;
    memcpy(out, t->cells + r * t->cols_cap, count * sizeof(fb_cell_t));
}

void fb_text_render_view(fb_console_t *fb) {
    fb_text_t *t = fb->text;
    if (!t || !fb->pixels || !fb->font) return;

    size_t view = fb->scrollback_view < t->history ? fb->scrollback_view : t->history;
    for (size_t row = 0; row < t->rows; row++)
        text_draw_row(fb, fb->pixels, row, t->history - view + row);
}
