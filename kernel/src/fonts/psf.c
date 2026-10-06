#include <fonts/psf.h>
#include <string.h>
#include <mm/kalloc.h>
#include <drivers/serial/serial.h>
#include <fonts/utf-8.h>
#include <stdbool.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t glyph_count;
    uint32_t bytes_per_glyph;
    uint32_t height;
    uint32_t width;
} __attribute__((packed)) psf2_header_t;

static psf_font_t *current_font = NULL;

psf_font_t *psf_get_current_font(void) {
    return current_font;
}

static void psf_build_unicode_map(psf_font_t *font) {
    if (!font->has_unicode_table) return;

    font->unicode_map = kmalloc(0x3000 * sizeof(uint16_t));
    if (!font->unicode_map) return;

    for (size_t i = 0; i < 0x3000; i++)
        font->unicode_map[i] = PSF_GLYPH_INVALID;

    if (font->is_psf2) {
        const uint8_t *p = font->unicode_table;
        const uint8_t *end = font->unicode_table + font->unicode_table_size;

        for (uint32_t glyph = 0; glyph < font->glyph_count && p < end; glyph++) {
            while (p < end) {
                if (*p == 0xFF) { p++; break; }
                if (*p == 0xFE) { p++; continue; }

                uint32_t cp;
                size_t len = utf8_decode(p, &cp);
                if (!len) { p++; continue; }
                if (cp < 0x3000) font->unicode_map[cp] = (uint16_t)glyph;
                p += len;
            }
        }
    } else {
        uint16_t *p = (uint16_t *)font->unicode_table;
        uint16_t *end = (uint16_t *)(font->unicode_table + font->unicode_table_size);
        uint32_t glyph = 0;

        while (p < end && glyph < font->glyph_count) {
            uint16_t cp = *p++;
            if (cp == 0xFFFF) {
                glyph++;
                continue;
            }
            if (cp < 0x3000) font->unicode_map[cp] = (uint16_t)glyph;
        }
    }
}

// the unicode table is whatever is left after the glyphs, the font still works without it
static void psf_load_unicode_table(psf_font_t *font, const uint8_t *b, size_t size, size_t off) {
    if (off >= size) return;

    font->unicode_table = kmalloc(size - off);
    if (!font->unicode_table) return;

    font->has_unicode_table = true;
    font->unicode_table_size = size - off;
    memcpy(font->unicode_table, b + off, font->unicode_table_size);
    psf_build_unicode_map(font);
}

static psf_font_t *psf_parse_psf1(const uint8_t *b, size_t size) {
    if (size < 4) return NULL;
    uint8_t mode = b[2];
    uint8_t charsize = b[3];
    size_t glyph_count = (mode & 0x01) ? 512 : 256;
    size_t glyph_bytes = glyph_count * charsize;

    // the header says how many glyphs follow, make sure the file really has them
    if (size < 4 + glyph_bytes) return NULL;

    psf_font_t *font = kmalloc(sizeof(*font));
    if (!font) return NULL;
    memset(font, 0, sizeof(*font));
    font->width = 8;
    font->height = charsize;
    font->bytes_per_row = 1;
    font->bytes_per_glyph = charsize;
    font->glyph_count = glyph_count;
    font->is_psf2 = false;

    font->glyphs = kmalloc(glyph_bytes);
    if (!font->glyphs) {
        kfree(font);
        return NULL;
    }
    memcpy(font->glyphs, b + 4, glyph_bytes);

    if (mode & 0x02)
        psf_load_unicode_table(font, b, size, 4 + glyph_bytes);
    return font;
}

static psf_font_t *psf_parse_psf2(const uint8_t *b, size_t size) {
    if (size < sizeof(psf2_header_t)) return NULL;
    const psf2_header_t *h = (const psf2_header_t *)b;
    if (h->magic != PSF2_MAGIC) return NULL;

    size_t glyph_bytes = (size_t)h->glyph_count * h->bytes_per_glyph;
    if (h->header_size > size || glyph_bytes > size - h->header_size) return NULL;

    psf_font_t *font = kmalloc(sizeof(*font));
    if (!font) return NULL;
    memset(font, 0, sizeof(*font));
    font->width = h->width;
    font->height = h->height;
    font->bytes_per_row = (h->width + 7) / 8;
    font->bytes_per_glyph = h->bytes_per_glyph;
    font->glyph_count = h->glyph_count;
    font->is_psf2 = true;

    font->glyphs = kmalloc(glyph_bytes);
    if (!font->glyphs) {
        kfree(font);
        return NULL;
    }
    memcpy(font->glyphs, b + h->header_size, glyph_bytes);

    if (h->flags & 0x01)
        psf_load_unicode_table(font, b, size, h->header_size + glyph_bytes);
    return font;
}

psf_font_t *psf_parse_font(const void *data, size_t size) {
    const uint8_t *b = data;
    if (size >= 2 && b[0] == PSF1_MAGIC0 && b[1] == PSF1_MAGIC1)
        return psf_parse_psf1(b, size);
    if (size >= 4 && *(const uint32_t *)b == PSF2_MAGIC)
        return psf_parse_psf2(b, size);
    return NULL;
}

uint16_t psf_lookup_glyph(const psf_font_t *font, uint32_t codepoint) {
    if (!font) return 0;
    if (codepoint < 0x80 && codepoint < font->glyph_count)
        return (uint16_t)codepoint;
    if (font->unicode_map && codepoint < 0x3000) {
        uint16_t g = font->unicode_map[codepoint];
        if (g != PSF_GLYPH_INVALID) return g;
    }
    if (codepoint < font->glyph_count) return (uint16_t)codepoint;
    return 0;
}

psf_font_t *psf_load_font(INode_t *inode) {
    size_t size = vfs_filesize(inode);
    uint8_t *buf = kmalloc(size);
    if (!buf) return NULL;

    // a short read would leave the parser looking at whatever kmalloc handed back
    psf_font_t *font = NULL;
    if (vfs_read_exact(inode, buf, size, 0) == 0)
        font = psf_parse_font(buf, size);
    kfree(buf);
    return font;
}

const uint8_t *psf_get_glyph_font(const psf_font_t *font, uint16_t code) {
    if (!font || code >= font->glyph_count) return NULL;
    return font->glyphs + (code * font->bytes_per_glyph);
}

bool psf_init(const char *font_path_abs) {
    path_t path = vfs_path_from_abs(font_path_abs);
    INode_t *inode;
    if (vfs_lookup(&path, &inode) < 0) return false;

    // only swap the font in once it loaded, a bad file shouldnt take the old one down with it
    psf_font_t *font = psf_load_font(inode);
    vfs_drop(inode);
    if (!font) return false;

    current_font = font;
    return true;
}
