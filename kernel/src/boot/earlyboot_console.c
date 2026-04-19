#include <boot/earlyboot_console.h>
#include <vendor/limine_bootloader/limine.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
    the purpose of this is quite unclear so it may look silly or entirely lazy

    the job of the earlyboot console is to display info on the screen that may be displayed 
    in serial, but to see if real hardware is doing okay

    it is slim, it does not have color escape sequences, it does not have a printf varient
    it is not advanced, it is designed to run before any important structures have started
    that is it, its more an earlyboot utility.
*/

extern volatile struct limine_framebuffer_request framebuffer_request;

static volatile uint32_t *fb_addr  = NULL;
static uint32_t  fb_width          = 0;
static uint32_t  fb_height         = 0;
static uint32_t  fb_pitch_u32      = 0;
static int       fb_initialized    = 0;

static uint32_t  cur_col           = 0;
static uint32_t  cur_row           = 0;
#define          FG_COLOR            0xFFFFFF
#define          BG_COLOR            0x000000

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    fb_addr[y * fb_pitch_u32 + x] = color;
}

static void draw_char(uint8_t c, uint32_t px, uint32_t py) {
    if (c < 0x20) c = 0x20;
    if (c > 0x7F) c = 0x7F;

    const uint8_t *g = font8x16[c - 0x20];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++)
            put_pixel(px + col, py + row,
                      (bits & (0x80u >> col)) ? FG_COLOR : BG_COLOR);
    }
}

void early_fb_init(void) {
    if (fb_initialized) return;

    struct limine_framebuffer_response *resp =
        (struct limine_framebuffer_response *)framebuffer_request.response;

    if (!resp || resp->framebuffer_count == 0) return;

    struct limine_framebuffer *fb = resp->framebuffers[0];

    fb_addr      = (volatile uint32_t *)(uintptr_t)fb->address;
    fb_width     = (uint32_t)fb->width;
    fb_height    = (uint32_t)fb->height;
    fb_pitch_u32 = (uint32_t)(fb->pitch / 4);
    fb_initialized = 1;

    // we cant clear here because the GDT is not set up and that would
    // be super dooper heavy
}

void early_fb_write_char(char c, uint32_t col, uint32_t row) {
    if (!fb_initialized) early_fb_init();
    if (!fb_addr) return;
    draw_char((uint8_t)c, col * 8, row * 16);
}

void early_fb_write_string(const char *str, uint32_t col, uint32_t row) {
    if (!fb_initialized) early_fb_init();
    if (!fb_addr || !str) return;

    uint32_t cx = col, cy = row;
    uint32_t cols = fb_width / 8;

    while (*str) {
        if (*str == '\n') { cx = col; cy++; }
        else {
            draw_char((uint8_t)*str, cx * 8, cy * 16);
            if (++cx >= cols) { cx = col; cy++; }
        }
        str++;
    }
}

static void early_fb_scroll(void) {
    uint32_t move_words = (fb_height - 16) * fb_pitch_u32;
    memmove((void *)fb_addr, (void *)(fb_addr + 16 * fb_pitch_u32), move_words * sizeof(uint32_t));
    
    uint32_t clear_start = (fb_height - 16) * fb_pitch_u32;
    uint32_t clear_words = 16 * fb_pitch_u32;
    for (uint32_t i = 0; i < clear_words; i++) {
        fb_addr[clear_start + i] = BG_COLOR;
    }
}

void early_fb_puts(const char *str) {
    if (!fb_initialized) early_fb_init();
    if (!fb_addr || !str) return;

    uint32_t cols = fb_width  / 8;
    uint32_t rows = fb_height / 16;

    while (*str) {
        if (*str == '\n') {
            cur_col = 0;
            if (++cur_row >= rows) {
                early_fb_scroll();
                cur_row = rows - 1;
            }
        } else {
            draw_char((uint8_t)*str, cur_col * 8, cur_row * 16);
            if (++cur_col >= cols) {
                cur_col = 0;
                if (++cur_row >= rows) {
                    early_fb_scroll();
                    cur_row = rows - 1;
                }
            }
        }
        str++;
    }
}

void early_fb_puts_reset(void) {
    cur_col = 0;
    cur_row = 0;
}

void early_fb_write_hex(uint64_t value, uint32_t col, uint32_t row) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) { buf[i] = hex[value & 0xF]; value >>= 4; }
    early_fb_write_string(buf, col, row);
}

void early_fb_write_dec(uint64_t value, uint32_t col, uint32_t row) {
    char buf[21];
    int pos = 20;
    buf[pos] = '\0';
    if (value == 0) { buf[--pos] = '0'; }
    else { while (value > 0) { buf[--pos] = '0' + (uint8_t)(value % 10); value /= 10; } }
    early_fb_write_string(&buf[pos], col, row);
}

// use sparingly pre-gdt/paging
void early_fb_clear(uint32_t color) {
    if (!fb_initialized) early_fb_init();
    if (!fb_addr) return;
    uint32_t total = fb_height * fb_pitch_u32;
    for (uint32_t i = 0; i < total; i++)
        fb_addr[i] = color;
}