#include <drivers/framebuffer/framebuffer.h>
#include <stdint.h>

void draw_bgra_image(uint32_t start_x, uint32_t start_y, uint32_t img_width, uint32_t img_height, const uint8_t* image_data) {
    if (!image_data) return;

    uint8_t* fb_addr = (uint8_t*)framebuffer_get_addr(0);
    uint64_t fb_width = framebuffer_get_width(0);
    uint64_t fb_height = framebuffer_get_height(0);
    uint64_t fb_pitch = framebuffer_get_pitch(0);
    uint64_t fb_bpp = framebuffer_get_bpp(0);

    if (fb_bpp != 32) return;

    for (uint32_t y = 0; y < img_height; y++) {
        // stop drawing if we hit the bottom edge of the screen
        if (start_y + y >= fb_height) break;

        for (uint32_t x = 0; x < img_width; x++) {
            // stop drawing if we hit the right edge of the screen
            if (start_x + x >= fb_width) break;

            // Calculate exact memory offsets
            // image_data is contiguous, so pitch is just width * 4
            uint64_t img_offset = (y * img_width + x) * 4; 
            uint64_t fb_offset = ((start_y + y) * fb_pitch) + ((start_x + x) * 4);

            *(uint32_t*)(fb_addr + fb_offset) = *(uint32_t*)(image_data + img_offset);
        }
    }
}