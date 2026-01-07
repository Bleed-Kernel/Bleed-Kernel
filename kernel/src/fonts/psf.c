#include <fonts/psf.h>
#include <string.h>
#include <stdint.h>
#include <fs/vfs.h>
#include <status.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <stdio.h>
#include <ansii.h>
#include <drivers/framebuffer/framebuffer.h>
#include <drivers/serial/serial.h>

#define PSF_MAGIC_0     0x36
#define PSF_MAGIC_1     0x04
#define PSF_HEADER_SIZE 4

#define PSF2_MAGIC 0x864AB572

psf_font_t *font = NULL;

/// @return current loaded font
psf_font_t *psf_get_current_font(){
    if (font) return font;
    else return NULL;
}

psf_font_t* psf_parse_font(const void *data, size_t size){
    const uint8_t *b = data;
    uint8_t mode = b[2];
    uint8_t charsize = b[3];

    size_t glyphcount = (mode & 0x01) ? 512 : 256;
    uint32_t width = 8;
    uint32_t height = charsize;
    uint32_t bytes_per_row = 1;
    size_t glyphbytes = bytes_per_row * height;

    psf_font_t *font = kmalloc(sizeof(psf_font_t));
    memset(font, 0, sizeof(*font));

    font->width = width;
    font->height = height;
    font->bytes_per_row = bytes_per_row;
    font->glyph_count = glyphcount;

    font->glyphs = kmalloc(glyphbytes * glyphcount);
    memcpy(font->glyphs, b + PSF_HEADER_SIZE, glyphbytes * glyphcount);

    // handle unicode table if present
    size_t remaining = size - (PSF_HEADER_SIZE + glyphbytes * glyphcount);
    if (remaining > 0) {
        font->has_unicode_table = true;
        font->unicode_table_size = remaining;
        font->unicode_table = kmalloc(remaining);
        memcpy(font->unicode_table, b + PSF_HEADER_SIZE + glyphbytes * glyphcount, remaining);
    }

    return font;
}

/// @brief loads a PSF font from an inode
/// @param inode file
/// @return font structe
psf_font_t* psf_load_font(INode_t* inode){
    if (!inode) return NULL;

    size_t filesize = vfs_filesize(inode);
    if (filesize == 0) return NULL;

    void *buffer = kmalloc(filesize);
    if (!buffer){
        serial_printf(LOG_ERROR "Out of Memory\n");
        return NULL;
    }

    long r = inode_read(inode, buffer, filesize, 0);
    if (r < 0 || (size_t)r != filesize) {
        kfree(buffer, filesize);
        serial_printf(LOG_ERROR "Bad Read\n");
        return NULL;
    }

    psf_font_t *font = NULL;

    const uint8_t *b = (const uint8_t *)buffer;

    if (filesize >= 2 && b[0] == PSF_MAGIC_0 && b[1] == PSF_MAGIC_1) {
        font = psf_parse_font(buffer, filesize);
    }

    else if (filesize >= 4 && *(const uint32_t *)b == PSF2_MAGIC) {
        serial_printf("Failed to load font becuase its PSF2 which we do not support, find a PSF1 font\n");
        return NULL;
    }
    else {
        serial_printf("Failed to load font, this means we cant print anything, oh no\n");
        return NULL;
    }

    kprintf(LOG_OK "Font Allocated at 0x%p (size: %lu)\n", buffer, filesize);
    kprintf(LOG_OK "Font Description:\n\tcharsize: %d\n\tglyph count: %ld\n", font->bytes_per_row, font->glyph_count);

    kfree(buffer, filesize);
    return font;
}

/// @brief get a glpyh from a font given a code
/// @param font target font
/// @param code unsigned 16 code
/// @return constant uint8 glyph
const uint8_t* psf_get_glyph_font(const psf_font_t *font, uint16_t code){
    if (!font) return NULL;

    // PSF1 font: glyphs are indexed from 0–255, skip control chars if you want
    if (code >= font->glyph_count) return NULL;

    // multiply by the **number of bytes per glyph**, not bytes_per_row
    return font->glyphs + code * font->bytes_per_row * font->height;
}

/// @brief free a font and all of its contents, including structure
/// @param font target font
void psf_free_font(psf_font_t *font){
    if (!font) return;
    if (font->glyphs) kfree(font->glyphs, (font->glyph_count)*font->bytes_per_row);
    if (font->unicode_table) kfree(font->unicode_table, font->unicode_table_size);
    kfree(font, sizeof(psf_font_t));
}

void psf_init(const char* font_path_abs){
    INode_t *font_file;
    path_t font_path;

    font_path = vfs_path_from_abs(font_path_abs);
    int lookup = vfs_lookup(&font_path, &font_file);
    if (lookup < 0) return;
    
    font = psf_load_font(font_file);
    inode_drop(font_file);
}