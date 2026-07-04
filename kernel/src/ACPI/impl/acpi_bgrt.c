#include <ACPI/acpi.h>
#include <ACPI/acpi_bgrt.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ansii.h>
#include <drivers/serial/serial.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/framebuffer/draw_image.h>
#include <mm/pmm.h>
#include <mm/kalloc.h>
#include "../acpi_priv.h"

static struct acpi_bgrt *bgrt = NULL;

bool acpi_init_bgrt(void) {
    bgrt = (struct acpi_bgrt *)acpi_find_sdt("BGRT");
    if (!bgrt) {
        serial_printf(LOG_WARN "BGRT: table not found, no firmware boot logo available\n");
        return false;
    }

    if (bgrt->image_type != BGRT_IMAGE_TYPE_BMP) {
        serial_printf(LOG_WARN "BGRT: unsupported image type %u\n", bgrt->image_type);
        bgrt = NULL;
        return false;
    }

    if (!bgrt->image_address) {
        serial_printf(LOG_WARN "BGRT: table present but image address is NULL\n");
        bgrt = NULL;
        return false;
    }

    serial_printf(LOG_OK "BGRT: found firmware logo (status=0x%x offset=%u,%u)\n",
                  bgrt->status, bgrt->image_offset_x, bgrt->image_offset_y);
    return true;
}

bool acpi_bgrt_present(void) {
    return bgrt != NULL;
}

bool acpi_bgrt_get_image(bgrt_image_t *out) {
    if (!bgrt || !out)
        return false;

    uint8_t *bmp = (uint8_t *)paddr_to_vaddr(bgrt->image_address);
    if (!bmp)
        return false;

    if (bmp[0] != 'B' || bmp[1] != 'M') {
        serial_printf(LOG_ERROR "BGRT: image at %p is not a valid BMP\n", (void *)bgrt->image_address);
        return false;
    }

    uint32_t data_offset     = *(uint32_t *)(bmp + 10);
    uint32_t dib_header_size = *(uint32_t *)(bmp + 14);

    if (dib_header_size < 40) {
        serial_printf(LOG_ERROR "BGRT: unsupported DIB header size %u (need BITMAPINFOHEADER)\n",
                      dib_header_size);
        return false;
    }

    int32_t  width  = *(int32_t  *)(bmp + 18);
    int32_t  height = *(int32_t  *)(bmp + 22);
    uint16_t bpp    = *(uint16_t *)(bmp + 28);
    uint32_t compression = *(uint32_t *)(bmp + 30);

    if (compression != 0) {
        serial_printf(LOG_ERROR "BGRT: compressed BMPs are not supported (compression=%u)\n", compression);
        return false;
    }

    if (bpp != 24 && bpp != 32) {
        serial_printf(LOG_ERROR "BGRT: unsupported bpp %u (only 24/32 supported)\n", bpp);
        return false;
    }

    if (width <= 0) {
        serial_printf(LOG_ERROR "BGRT: invalid width %d\n", width);
        return false;
    }

    bool top_down = height < 0;
    uint32_t abs_height = top_down ? (uint32_t)(-height) : (uint32_t)height;
    if (abs_height == 0) {
        serial_printf(LOG_ERROR "BGRT: invalid height 0\n");
        return false;
    }

    // BMP rows are padded to a 4-byte boundary.
    uint32_t row_stride = ((((uint32_t)width) * (bpp / 8)) + 3) & ~3u;

    out->width      = (uint32_t)width;
    out->height     = abs_height;
    out->bpp        = bpp;
    out->offset_x   = bgrt->image_offset_x;
    out->offset_y   = bgrt->image_offset_y;
    out->row_stride = row_stride;
    out->top_down   = top_down;
    out->pixel_data = bmp + data_offset;

    return true;
}

const uint8_t *acpi_bgrt_row_ptr(const bgrt_image_t *img, uint32_t y) {
    if (!img || !img->pixel_data || y >= img->height)
        return NULL;

    uint32_t row = img->top_down ? y : (img->height - 1 - y);
    return (const uint8_t *)img->pixel_data + ((uintptr_t)row * img->row_stride);
}

bool acpi_bgrt_display_logo(void) {
    bgrt_image_t img;
    if (!acpi_bgrt_get_image(&img))
        return false;

    if (framebuffer_get_bpp(0) != 32) {
        serial_printf(LOG_ERROR "BGRT: display_logo requires a 32bpp framebuffer\n");
        return false;
    }

    size_t buf_size = (size_t)img.width * (size_t)img.height * 4;
    uint8_t *buf = kmalloc(buf_size);
    if (!buf) {
        serial_printf(LOG_ERROR "BGRT: failed to allocate %u bytes for logo conversion\n",
                      (unsigned)buf_size);
        return false;
    }

    // draw_bgra_image wants a flat, top-down, 4-bytes-per-pixel buffer with
    // no row padding, we have to convert it tho because BMP doesnt guarantee it
    for (uint32_t y = 0; y < img.height; y++) {
        const uint8_t *src_row = acpi_bgrt_row_ptr(&img, y);
        uint8_t *dst_row = buf + (size_t)y * img.width * 4;

        if (!src_row) {
            memset(dst_row, 0, (size_t)img.width * 4);
            continue;
        }

        if (img.bpp == 32) {
            memcpy(dst_row, src_row, (size_t)img.width * 4);
        } else {
            for (uint32_t x = 0; x < img.width; x++) {
                dst_row[x * 4 + 0] = src_row[x * 3 + 0]; // B
                dst_row[x * 4 + 1] = src_row[x * 3 + 1]; // G
                dst_row[x * 4 + 2] = src_row[x * 3 + 2]; // R
                dst_row[x * 4 + 3] = 0xFF;               // A
            }
        }
    }

    draw_bgra_image(img.offset_x, img.offset_y, img.width, img.height, buf);
    kfree(buf);

    serial_printf(LOG_OK "BGRT: drew %ux%u firmware logo at (%u,%u)\n",
                  img.width, img.height, img.offset_x, img.offset_y);
    return true;
}