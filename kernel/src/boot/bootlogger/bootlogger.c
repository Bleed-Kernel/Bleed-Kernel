#include <boot/bootlogger/bootlogger.h>
#include <vendor/limine_bootloader/limine.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

#define MAX_BACKBUFFER_WORDS (2560 * 1440)          // staticness for speed, yeah early in boot we have no fancy tricks available
static uint32_t s_backbuffer[MAX_BACKBUFFER_WORDS];

static volatile uint32_t *s_fb       = NULL;
static uint32_t  s_width             = 0;
static uint32_t  s_height            = 0;
static uint32_t  s_pitch32           = 0;
static uint32_t  s_cols              = 0;
static uint32_t  s_rows              = 0;
static int       s_ready             = 0;

static uint32_t  s_cur_col           = 0;
static uint32_t  s_cur_row           = 0;

static uint32_t  s_fg                = BCOL_WHITE;
static uint32_t  s_bg                = BCOL_BLACK;

static bool      bl_ready            = false;

static void draw_glyph(uint8_t c, uint32_t px, uint32_t py,
                        uint32_t fg, uint32_t bg) {
    if (c < 0x20 || c > 0x7F) c = 0x20;

    const uint8_t *g = font8x16[c - 0x20];
    for (uint32_t row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        uint32_t offset = (py + row) * s_pitch32 + px;

        uint32_t *dst_row = &s_backbuffer[offset];
        volatile uint32_t *fb_row = &s_fb[offset];

        for (uint32_t col = 0; col < FONT_W; col++) {
            dst_row[col] = (bits & (0x80u >> col)) ? fg : bg;
        }

        for (uint32_t col = 0; col < FONT_W; col++) {
            fb_row[col] = dst_row[col];
        }
    }
}

static void scroll_up_one_row(void) {
    uint32_t row_words  = s_pitch32 * FONT_H;
    uint32_t total_rows = s_rows - 1;
    uint32_t shift_words = row_words * total_rows;

    uint32_t *dst = s_backbuffer;
    uint32_t *src = s_backbuffer + row_words;
    for (uint32_t i = 0; i < shift_words; i++) {
        dst[i] = src[i];
    }

    uint32_t *last = s_backbuffer + shift_words;
    for (uint32_t i = 0; i < row_words; i++) {
        last[i] = s_bg;
    }

    uint32_t total_words = s_pitch32 * s_height;
    volatile uint32_t *fb_dst = s_fb;
    for (uint32_t i = 0; i < total_words; i++) {
        fb_dst[i] = s_backbuffer[i];
    }
}

static void advance_cursor(void) {
    if (++s_cur_col >= s_cols) {
        s_cur_col = 0;
        if (++s_cur_row >= s_rows) {
            scroll_up_one_row();
            s_cur_row = s_rows - 1;
        }
    }
}

static void newline(void) {
    s_cur_col = 0;
    if (++s_cur_row >= s_rows) {
        scroll_up_one_row();
        s_cur_row = s_rows - 1;
    }
}

void bconsole_init(void) {
    if (s_ready) return;
    if (!g_gbi.framebuffer.present || !g_gbi.framebuffer.address) return;

    s_fb      = (volatile uint32_t *)(uintptr_t)g_gbi.framebuffer.address;
    s_width   = (uint32_t)g_gbi.framebuffer.width;
    s_height  = (uint32_t)g_gbi.framebuffer.height;

    if (g_gbi.framebuffer.bpp != 32) return;
    s_pitch32 = (uint32_t)(g_gbi.framebuffer.pitch / 4);

    if ((s_pitch32 * s_height) > MAX_BACKBUFFER_WORDS) return;

    s_cols  = s_width  / FONT_W;
    s_rows  = s_height / FONT_H;

    if (s_cols == 0) s_cols = 1;
    if (s_rows == 0) s_rows = 1;

    s_cur_col = 0;
    s_cur_row = 0;
    s_fg      = BCOL_WHITE;
    s_bg      = BCOL_BLACK;
    s_ready   = 1;

    bl_ready = true;
}

void bconsole_clear(uint32_t color) {
    if (!s_ready) return;
    if (!s_fb)    return;

    uint32_t total = s_height * s_pitch32;
    for (uint32_t i = 0; i < total; i++) {
        s_backbuffer[i] = color;
    }

    for (uint32_t i = 0; i < total; i++) {
        s_fb[i] = s_backbuffer[i];
    }

    s_cur_col = 0;
    s_cur_row = 0;
}

void bset_color(uint32_t fg, uint32_t bg) {
    s_fg = fg;
    s_bg = bg;
}

void breset_color(void) {
    s_fg = BCOL_WHITE;
    s_bg = BCOL_BLACK;
}

void bconsole_reset_cursor(void) {
    s_cur_col = 0;
    s_cur_row = 0;
}

void bconsole_set_cursor(uint32_t col, uint32_t row) {
    s_cur_col = (col < s_cols) ? col : s_cols - 1;
    s_cur_row = (row < s_rows) ? row : s_rows - 1;
}

void bconsole_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels) {
    if (!s_ready) return;
    if (!s_fb)    return;
    if (!pixels)  return;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t fb_y = y + row;
        if (fb_y >= s_height) break;

        uint32_t offset = fb_y * s_pitch32 + x;
        uint32_t *dst_row = &s_backbuffer[offset];
        volatile uint32_t *fb_row = &s_fb[offset];
        const uint32_t *src_row = &pixels[row * w];

        uint32_t max_col = w;
        if (x + max_col > s_width) max_col = s_width - x;

        for (uint32_t col = 0; col < max_col; col++) {
            dst_row[col] = src_row[col];
        }

        for (uint32_t col = 0; col < max_col; col++) {
            fb_row[col] = dst_row[col];
        }
    }
}

void bconsole_reserve_rows(uint32_t pixel_height) {
    if (!s_ready) return;

    uint32_t rows = (pixel_height + FONT_H - 1) / FONT_H;
    if (rows > s_rows) rows = s_rows - 1;
    if (rows > s_cur_row) s_cur_row = rows;
    s_cur_col = 0;
}

static void emit_char(char c) {
    if (!s_ready) return;
    if (!s_fb)    return;

    if (c == '\n') { newline();   return; }
    if (c == '\r') { s_cur_col = 0; return; }
    if (c == '\t') {
        uint32_t next = (s_cur_col + 8) & ~7u;
        while (s_cur_col < next) advance_cursor();
        return;
    }

    draw_glyph((uint8_t)c,
               s_cur_col * FONT_W,
               s_cur_row * FONT_H,
               s_fg, s_bg);
    advance_cursor();
}

static void emit_string(const char *s) {
    if (!s) { emit_string("(null)"); return; }
    while (*s) emit_char(*s++);
}

static void emit_uint_dec(uint64_t v) {
    char buf[21];
    int  pos = 20;
    buf[pos] = '\0';
    if (v == 0) { buf[--pos] = '0'; }
    else { while (v) { buf[--pos] = '0' + (char)(v % 10); v /= 10; } }
    emit_string(&buf[pos]);
}

static void emit_int_dec(int64_t v) {
    if (v < 0) { emit_char('-'); emit_uint_dec((uint64_t)(-v)); }
    else       { emit_uint_dec((uint64_t)v); }
}

static void emit_uint_hex(uint64_t v, int upper) {
    static const char lo[] = "0123456789abcdef";
    static const char up[] = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    char buf[17];
    int  pos = 16;
    buf[pos] = '\0';
    if (v == 0) { buf[--pos] = '0'; }
    else { while (v) { buf[--pos] = digits[v & 0xF]; v >>= 4; } }
    emit_string(&buf[pos]);
}

void bvprintf(const char *fmt, va_list ap) {
    if (!fmt) return;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit_char(*p); continue; }

        p++;

        int  is_long = 0;
        if (*p == 'l') { is_long++; p++; }
        if (*p == 'l') { is_long++; p++; }
        if (*p == 'z') { is_long  = 1; p++; }

        switch (*p) {
        case 'c':
            emit_char((char)va_arg(ap, int));
            break;
        case 's':
            emit_string(va_arg(ap, const char *));
            break;
        case 'd':
        case 'i': {
            int64_t v = (is_long >= 2) ? (int64_t)va_arg(ap, long long)
                      : (is_long == 1) ? (int64_t)va_arg(ap, long)
                      :                  (int64_t)va_arg(ap, int);
            emit_int_dec(v);
            break;
        }
        case 'u': {
            uint64_t v = (is_long >= 2) ? (uint64_t)va_arg(ap, unsigned long long)
                       : (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long)
                       :                  (uint64_t)va_arg(ap, unsigned int);
            emit_uint_dec(v);
            break;
        }
        case 'x': {
            uint64_t v = (is_long >= 2) ? (uint64_t)va_arg(ap, unsigned long long)
                       : (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long)
                       :                  (uint64_t)va_arg(ap, unsigned int);
            emit_uint_hex(v, 0);
            break;
        }
        case 'X': {
            uint64_t v = (is_long >= 2) ? (uint64_t)va_arg(ap, unsigned long long)
                       : (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long)
                       :                  (uint64_t)va_arg(ap, unsigned int);
            emit_uint_hex(v, 1);
            break;
        }
        case 'p': {
            emit_string("0x");
            emit_uint_hex((uint64_t)(uintptr_t)va_arg(ap, void *), 1);
            break;
        }
        case '%':
            emit_char('%');
            break;
        default:
            emit_char('%');
            emit_char(*p);
            break;
        }
    }
}

void bprintf(const char *fmt, ...) {
    if (!bl_ready) return;
    va_list ap;
    va_start(ap, fmt);
    bvprintf(fmt, ap);
    va_end(ap);
}