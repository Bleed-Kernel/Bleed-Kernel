#include <fs/vfs.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/draw_bgra_image.h>
#include <ACPI/acpi_boot_logo.h>
#include <media/bmp/bmp.h>
#include <mm/kalloc.h>
#include <kernel/bootargs.h>

static void draw_uefi_bmp(void* bmp_data) {
    uint8_t* data = (uint8_t*)bmp_data;

    if (data[0] != 'B' || data[1] != 'M') return;
    uint32_t data_offset    = *(uint32_t*)(data + 10);
    int32_t width           = *(int32_t*)(data + 18);
    int32_t height          = *(int32_t*)(data + 22);
    uint16_t bpp            = *(uint16_t*)(data + 28);

    if (bpp != 24 && bpp != 32) return;

    int is_top_down = (height < 0);
    if (is_top_down) height = -height;

    size_t bgra_size = width * height * 4;
    uint8_t* bgra_buffer = kmalloc(bgra_size);
    if (!bgra_buffer) return;

    uint32_t row_stride = ((width * bpp / 8) + 3) & ~3;
    uint8_t* pixel_data = data + data_offset;

    // convert BMP pixels to BGRA
    for (int y = 0; y < height; y++) {
        int src_y = is_top_down ? y : (height - 1 - y);

        uint8_t* src_row = pixel_data + (src_y * row_stride);
        uint8_t* dst_row = bgra_buffer + (y * width * 4);

        for (int x = 0; x < width; x++) {
            int src_idx = x * (bpp / 8);
            int dst_idx = x * 4;

            dst_row[dst_idx] = src_row[src_idx]; // Blue
            dst_row[dst_idx + 1] = src_row[src_idx + 1]; // Green
            dst_row[dst_idx + 2] = src_row[src_idx + 2]; // Red
            dst_row[dst_idx + 3] = (bpp == 32) ? src_row[src_idx + 3] : 255; // Alpha
            }
    }

    uint64_t fb_width = framebuffer_get_width(0);
    uint64_t fb_height = framebuffer_get_height(0);

    uint32_t start_x = (fb_width > (uint32_t)width) ? (fb_width - width) / 2 : 0;
    uint32_t start_y = (fb_height > (uint32_t)height) ? (fb_height - height) / 2 : 0;

    draw_bgra_image(start_x, start_y, width, height, bgra_buffer);

    kfree(bgra_buffer, bgra_size);
}

/// @brief splash screen helper
/// @param image_path image
/// @param img_width x
/// @param img_height y
void display_splash_screen(const char* image_path, uint32_t img_width, uint32_t img_height) {

    bmp_file_t* bootloader_image = ACPI_get_boot_logo();
    if (bootloader_image != NULL && !bootargs_is("preferred-splash", "bleed")) {
        draw_uefi_bmp((void*)bootloader_image);
        return;
    }

    int fd = vfs_open(image_path, O_RDONLY);
    if (fd < 0) return;

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

    uint32_t start_x = (fb_width > img_width) ? (fb_width - img_width) / 2 : 0;
    uint32_t start_y = (fb_height > img_height) ? (fb_height - img_height) / 2 : 0;

    draw_bgra_image(start_x, start_y, img_width, img_height, image_buffer);

    kfree(image_buffer, image_size);
}