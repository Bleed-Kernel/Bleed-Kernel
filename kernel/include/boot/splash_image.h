#pragma once
#include <stdint.h>
#include <stdbool.h>

bool display_splash_screen(const char* image_path, uint32_t img_width, uint32_t img_height);

// the bar under bleeds own logo is its red, under a firmware logo its white
#define SPLASH_BAR_BLEED        0xC1121F
#define SPLASH_TRACK_BLEED      0x2A0A0E
#define SPLASH_BAR_FIRMWARE     0xFFFFFF
#define SPLASH_TRACK_FIRMWARE   0x2A2A2A

// clear the screen, fade a top down bgra image in at x y and start the progress bar under it
void splash_show(const uint8_t *image, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t bar_fill, uint32_t bar_track);

// one more boot stage is done, the bar under the logo moves along. does nothing with no splash up
void splash_progress(void);

// boot is over, fill whatever is left of the bar
void splash_finish(void);
