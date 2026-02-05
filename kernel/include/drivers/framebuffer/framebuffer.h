#pragma once

#include <fonts/psf.h>
#include <drivers/framebuffer/framebuffer_console.h>
#include <string.h>
#include <stdint.h>
#include <mm/spinlock.h>

typedef struct {
    uint32_t fg;
    uint32_t bg;
    int esc;
    int csi;
    int params[16];
    int param_count;
    int substate;
    int subparams[4];
} ansii_state_t;


/// @return pointer to framebuffer
void* framebuffer_get_addr(int idx);

/// @return bytes between 2 scanlines
uint64_t framebuffer_get_pitch(int idx);

/// @return framebuffer (x)
uint64_t framebuffer_get_width(int idx);

/// @return framebuffer (y)
uint64_t framebuffer_get_height(int idx);

uint64_t framebuffer_get_bpp(int idx);

/// @brief write a character to the framebuffer
/// @param font text font
/// @param c character
/// @param fg foreground colour
/// @param bg background colour
void framebuffer_put_char(fb_console_t *fb, uint32_t c);

/// @brief recursivly write characters from a string to the framebuffer
/// @param str target
void framebuffer_write_string(
    fb_console_t *fb,
    ansii_state_t *ansi,
    const char *str,
    spinlock_t *framebuffer_lock
);

/// @brief evaluate c and track its ansii state
/// @param c target
void framebuffer_ansi_char(fb_console_t *fb, spinlock_t *framebuffer_lock, ansii_state_t *st, uint32_t c);

/// @brief clear the framebuffer
/// @param color bg colour to clear with
void framebuffer_clear(uint32_t *pixels, uint64_t width, uint64_t height, uint64_t pitch, uint32_t colour);

uint32_t* framebuffer_get_buffer(void);

/// @brief Draw a single line of text centered on the framebuffer
/// @param fb framebuffer console
/// @param ansi ansi state
/// @param line text to draw
/// @param y line index (0 = top)
/// @param framebuffer_lock optional framebuffer lock
/// @param fg foreground color
/// @param bg background color
static inline void framebuffer_write_centered_line(
    fb_console_t *fb,
    ansii_state_t *ansi,
    const char *line,
    uint64_t y,
    spinlock_t *framebuffer_lock,
    uint32_t fg,
    uint32_t bg
) {
    if (!fb || !line) return;

    uint64_t fb_width = framebuffer_get_width(0);
    size_t len = strlen(line);

    // Compute starting x for centering
    int64_t x_start = (int64_t)fb_width / 2 - (int64_t)len / 2;
    if (x_start < 0) x_start = 0;

    // Move cursor to y
    fb->cursor_y = y;
    fb->cursor_x = x_start;

    // Write each char with colors
    for (size_t i = 0; i < len; i++) {
        // Set colors
        fb->fg = fg;
        fb->bg = bg;

        framebuffer_ansi_char(fb, framebuffer_lock, ansi, line[i]);
    }
}

/// @brief Draw multiple lines centered vertically and horizontally
/// @param fb framebuffer console
/// @param ansi ansi state
/// @param lines array of null-terminated strings
/// @param count number of lines
/// @param framebuffer_lock optional framebuffer lock
/// @param fg foreground color
/// @param bg background color
static inline void framebuffer_write_centered(
    fb_console_t *fb,
    ansii_state_t *ansi,
    const char **lines,
    size_t count,
    spinlock_t *framebuffer_lock,
    uint32_t fg,
    uint32_t bg
) {
    if (!fb || !lines || count == 0) return;

    uint64_t fb_height = framebuffer_get_height(0);

    int64_t y_start = (int64_t)fb_height / 2 - (int64_t)count / 2;
    if (y_start < 0) y_start = 0;

    for (size_t i = 0; i < count; i++) {
        framebuffer_write_centered_line(fb, ansi, lines[i], y_start + i, framebuffer_lock, fg, bg);
    }
}