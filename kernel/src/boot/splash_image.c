#include <fs/vfs.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/draw_image.h>
#include <boot/splash_image.h>
#include <kernel/bootargs.h>
#include <mm/kalloc.h>
#include <drivers/pit/pit.h>
#include <ACPI/acpi_hpet.h>
#include <mm/spinlock.h>
#include <stdbool.h>

#define SPLASH_STEPS        21      // boot stages that report in after the splash goes up

#define FADE_STEPS          24
#define FADE_STEP_MS        40

#define BAR_W               200
#define BAR_H               4
#define BAR_GAP             128
#define BAR_SWEEP_US        900     // per pixel when the bar is run up to full at the end
#define BAR_HOLD_MS         150     // how long its left full before the console takes the screen
#define BAR_HOLD_SLICE_US   999     // a millisecond, kept under one so the wait doesnt yield
#define BAR_MARGIN          16      // from the bottom of the screen, if the logo sits too low for the gap

static struct {
    bool      on;
    uint32_t *fb;
    uint32_t  width, height, pitch32;
    uint32_t  bar_x, bar_y;
    uint32_t  bar_fill, bar_track;
    uint32_t  steps;
} splash;

static void put(uint32_t x, uint32_t y, uint32_t colour) {
    if (x < splash.width && y < splash.height)
        splash.fb[y * splash.pitch32 + x] = colour;
}

static void fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour) {
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++)
            put(x + col, y + row, colour);
    }
}

static uint32_t fade_channel(uint32_t colour, uint32_t shift, uint32_t level) {
    return (((colour >> shift) & 0xFF) * level / FADE_STEPS) << shift;
}

// level out of FADE_STEPS, the last one is the image as it is
static void logo_draw(const uint8_t *image, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t level) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t colour = *(const uint32_t *)(image + (y * w + x) * 4);

            put(x0 + x, y0 + y, fade_channel(colour, 16, level) |
                                fade_channel(colour, 8, level) |
                                fade_channel(colour, 0, level));
        }
    }
}

// how many pixels of the bar the stages so far are worth
static uint32_t bar_done(void) {
    return splash.steps >= SPLASH_STEPS ? BAR_W : splash.steps * BAR_W / SPLASH_STEPS;
}

static void bar_draw(uint32_t done) {
    fill(splash.bar_x, splash.bar_y, done, BAR_H, splash.bar_fill);
    fill(splash.bar_x + done, splash.bar_y, BAR_W - done, BAR_H, splash.bar_track);
}

void splash_progress(void) {
    if (!splash.on) return;
    splash.steps++;
    bar_draw(bar_done());
}

// how many stages report in depends on the machine, so the count alone can leave the bar well
// short. whatever is left is run up to the end here and held there, its always seen full
void splash_finish(void) {
    if (!splash.on) return;
    splash.on = false;
    unsigned long flags = irq_push();

    for (uint32_t done = bar_done(); done <= BAR_W; done++) {
        bar_draw(done);
        wait_us(BAR_SWEEP_US);
    }
    for (uint32_t i = 0; i < BAR_HOLD_MS; i++)
        wait_us(BAR_HOLD_SLICE_US);

    irq_restore(flags);
}

void splash_show(const uint8_t *image, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t bar_fill, uint32_t bar_track) {
    // a verbose boot has the log all over the screen, the logo goes up plain and thats that
    if (bootargs_has("verbose") || framebuffer_get_bpp(0) != 32) {
        draw_bgra_image(x, y, w, h, image);
        return;
    }

    splash.fb      = (uint32_t *)framebuffer_get_addr(0);
    splash.width   = (uint32_t)framebuffer_get_width(0);
    splash.height  = (uint32_t)framebuffer_get_height(0);
    splash.pitch32 = (uint32_t)(framebuffer_get_pitch(0) / 4);

    fill(0, 0, splash.width, splash.height, 0x000000);

    bool timed = true;
    for (uint32_t level = 1; level <= FADE_STEPS && timed; level++) {
        logo_draw(image, x, y, w, h, level);
        timed = pit_wait_ms(FADE_STEP_MS);
    }
    logo_draw(image, x, y, w, h, FADE_STEPS);

    // firmware puts its logo wherever it likes, the bar still has to land on the screen
    splash.bar_x = splash.width > BAR_W ? (splash.width - BAR_W) / 2 : 0;
    splash.bar_y = y + h + BAR_GAP;
    if (splash.bar_y + BAR_H + BAR_MARGIN > splash.height)
        splash.bar_y = splash.height > BAR_H + BAR_MARGIN ? splash.height - BAR_H - BAR_MARGIN : 0;

    splash.bar_fill  = bar_fill;
    splash.bar_track = bar_track;
    splash.steps     = 0;
    splash.on        = true;
    bar_draw(0);
}

/// @brief splash screen helper
/// @param image_path image
/// @param img_width x
/// @param img_height y
bool display_splash_screen(const char* image_path, uint32_t img_width, uint32_t img_height) {
    int fd = vfs_open(image_path, O_RDONLY);
    if (fd < 0) return false;

    // alloc buffer
    size_t image_size = img_width * img_height * 4; 
    uint8_t* image_buffer = kmalloc(image_size);
    if (!image_buffer) {
        vfs_close(fd);
        return false;
    }

    vfs_read(fd, image_buffer, image_size);
    vfs_close(fd);

    uint64_t fb_width = framebuffer_get_width(0);
    uint64_t fb_height = framebuffer_get_height(0);

    // Calculate centering offsets
    uint32_t start_x = (fb_width > img_width) ? (fb_width - img_width) / 2 : 0;
    uint32_t start_y = (fb_height > img_height) ? (fb_height - img_height) / 2 : 0;

    splash_show(image_buffer, start_x, start_y, img_width, img_height, SPLASH_BAR_BLEED, SPLASH_TRACK_BLEED);
    kfree(image_buffer);
    return true;
}