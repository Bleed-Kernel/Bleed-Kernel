#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <fs/vfs.h>

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

#define PSF2_MAGIC  0x864AB572

#define PSF_UNICODE_MAX 0x110000
#define PSF_GLYPH_INVALID 0xFFFF

typedef struct psf_font {
    uint8_t  *glyphs;
    size_t    glyph_count;

    uint32_t  width;
    uint32_t  height;
    uint32_t  bytes_per_row;
    uint32_t  bytes_per_glyph;

    bool      has_unicode_table;
    uint8_t  *unicode_table;
    size_t    unicode_table_size;

    uint16_t *unicode_map;

    bool      is_psf2;
} psf_font_t;

/// @return current loaded font
psf_font_t *psf_get_current_font();

/// @brief parses font file raw data
/// @param data raw data of font file
/// @param size size of the data
/// @return font structure
psf_font_t* psf_parse_font(const void *data, size_t size);

/// @brief loads a PSF font from an inode
/// @param inode file
/// @return font structe
psf_font_t* psf_load_font(INode_t *inode);

/// @brief get a glpyh from a font given a code
/// @param font target font
/// @param code unsigned 16 code
/// @return constant uint8 glyph
const uint8_t* psf_get_glyph_font(const psf_font_t *font, uint16_t code);

uint16_t psf_lookup_glyph(const psf_font_t *font, uint32_t codepoint);

/// @brief free a font and all of its contents, including structure
/// @param font target font
void psf_free_font(psf_font_t *font);

/// @brief load the inital font from PSF
/// @param font_path_abs target initial PSF
void psf_init(const char* font_path_abs);