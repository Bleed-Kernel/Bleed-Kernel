#include <fs/vfs.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/draw_image.h>
#include <mm/kalloc.h>

/// @brief splash screen helper
/// @param image_path image
/// @param img_width x
/// @param img_height y
void display_splash_screen(const char* image_path, uint32_t img_width, uint32_t img_height) {
    int fd = vfs_open(image_path, O_RDONLY);
    if (fd < 0) return;

    // alloc buffer
    size_t image_size = img_width * img_height * 4; 
    uint8_t* image_buffer = kmalloc(image_size);
    if (!image_buffer) {
        vfs_close(fd);
        return;
    }

    vfs_read(fd, image_buffer, image_size);
    vfs_close(fd);

    uint64_t fb_width = framebuffer_get_width(0);
    uint64_t fb_height = framebuffer_get_height(0);

    // Calculate centering offsets
    uint32_t start_x = (fb_width > img_width) ? (fb_width - img_width) / 2 : 0;
    uint32_t start_y = (fb_height > img_height) ? (fb_height - img_height) / 2 : 0;

    draw_bgra_image(start_x, start_y, img_width, img_height, image_buffer);

    kfree(image_buffer, image_size);
}