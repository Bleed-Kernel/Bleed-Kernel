// bitmap header from https://www.ece.ualberta.ca/~elliott/ee552/studentAppNotes/2003_w/misc/bmp_file_format/bmp_file_format.htm
#pragma once
#include <stdint.h>
#include <stddef.h>

struct bmp_header{
    char        header[2];      // should just be "BM"
    uint32_t    file_sz;
    uint32_t    reserved;
    uint32_t    data_offset;    // offset from the start of the file to beginning of the bitmap image data
}__attribute__((packed));

struct bmp_colour_table{ //scale refers to intensity
    uint8_t red_scale;
    uint8_t green_scale;
    uint8_t blue_scale;
    uint8_t reserved;
};

typedef struct bmp_file{
    struct bmp_header bitmap_header;

    uint32_t    info_header_sz;
    uint32_t    width;
    uint32_t    height;
    uint16_t    planes;
    uint16_t    bpp;
    uint32_t    compression;
    uint32_t    image_sz;
    uint32_t    width_pixels_per_meter;
    uint32_t    height_pixels_per_meter;
    uint32_t    colours_used;
    uint32_t    important_colours;

    struct bmp_colour_table color_table;
}__attribute__((packed)) bmp_file_t;