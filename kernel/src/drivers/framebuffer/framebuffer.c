#include <boot/bootloader_interface/generic_bootloader.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/blit.h>
#include <devices/type/tty_device.h>
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