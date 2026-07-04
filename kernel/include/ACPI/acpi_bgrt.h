#pragma once
#include <ACPI/acpi.h>
#include <stdint.h>
#include <stdbool.h>

struct acpi_bgrt {
    struct acpi_sdt header;
    uint16_t version;
    uint8_t  status;
    uint8_t  image_type;
    uint64_t image_address;
    uint32_t image_offset_x;
    uint32_t image_offset_y;
} __attribute__((packed));

#define BGRT_STATUS_DISPLAYED (1 << 0)
#define BGRT_IMAGE_TYPE_BMP   0

typedef struct {
    uint32_t width;
    uint32_t height;
    uint16_t bpp;
    uint32_t offset_x;
    uint32_t offset_y;
    void    *pixel_data;
    uint32_t row_stride;
    bool     top_down;
} bgrt_image_t;

bool acpi_init_bgrt(void);
bool acpi_bgrt_present(void);
bool acpi_bgrt_get_image(bgrt_image_t *out);
const uint8_t *acpi_bgrt_row_ptr(const bgrt_image_t *img, uint32_t y);

bool acpi_bgrt_display_logo(void);