#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <cpu/percpu.h>
#include <sched/scheduler.h>
#include <drivers/framebuffer/framebuffer.h>
#include <kernel/exception/panic.h>
#include <kernel/exception/panic_screen.h>
#include <kernel/exception/stack_trace.h>
#include <vendor/qrcodegen/qrcodegen.h>

// these fonts are baked in theyre you can use a simple py script to turn
// any font into this bpf font
#define LOGO_PATH       "initrd/boot/splash.bgra"
#define TITLE_PATH      "initrd/boot/panic-title.bpf"
#define SUB_PATH        "initrd/boot/panic-sub.bpf"
#define MONO_PATH       "initrd/boot/panic-mono.bpf"
#define LOGO_SIZE       256

#define COL_TITLE       0xC1121F
#define COL_TEXT        0xFFFFFF
#define COL_DIM         0xFFFFFF
#define COL_QR_LIGHT    0xF2F2F2

#define MARGIN          24
#define INFO_MAX        5
#define LABEL_W         13
#define COL_CHARS_MAX   72
#define QR_MODULE_MIN   2
#define QR_MODULE_MAX   5
#define QR_QUIET        4
#define QR_GAP          40
#define LOGO_FULL_H     1000

#define PANIC_TITLE     "Kernel Panic!"
#define PANIC_SUB1      "Bleed ran into a problem and needed to stop"
#define PANIC_SUB2      "Please power off the computer with its power button"
#define PANIC_SCAN      "Scan the QR for more information"
#define PANIC_NO_QR     "A full report has been dumped into COM3"
#define PANIC_SITE      "bleedkernel.com/panic.html"

typedef struct {
    char     magic[4];
    uint16_t size, line, ascent, first, count, pad;
} __attribute__((packed)) bpf_header_t;

typedef struct {
    uint16_t w, h;
    int16_t  left, top;
    uint16_t advance, pad;
    uint32_t offset;
} __attribute__((packed)) bpf_glyph_t;

typedef struct {
    const bpf_header_t *head;
    const bpf_glyph_t  *glyphs;
    const uint8_t      *pixels;
} bpf_font_t;

typedef struct {
    const char *label;
    char        value[160];
} info_row_t;

static struct {
    bool        ready;
    uint8_t    *logo;
    bpf_font_t  title, sub, mono;
    uint32_t   *fb;
    int         width, height, pitch32;
    int         scale;
} ps;

// static so the panic doesnt put 8k of qr code on an 8k kernel stack
static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];

static void *load_file(const char *path, size_t *size_out) {
    INode_t *file = NULL;
    path_t filepath = vfs_path_from_abs(path);
    if (vfs_lookup(&filepath, &file) < 0 || !file)
        return NULL;

    size_t size = vfs_filesize(file);
    void  *buf  = size ? kmalloc(size) : NULL;
    if (buf && inode_read(file, buf, size, 0) != (long)size) {
        kfree(buf);
        buf = NULL;
    }
    vfs_drop(file);

    *size_out = size;
    return buf;
}

static bool font_load(bpf_font_t *font, const char *path) {
    size_t size;
    uint8_t *data = load_file(path, &size);
    if (!data) return false;

    const bpf_header_t *head = (const bpf_header_t *)data;
    if (size < sizeof(*head) || memcmp(head->magic, "BPFN", 4) != 0 || !head->count)
        goto bad;

    size_t table = sizeof(*head) + (size_t)head->count * sizeof(bpf_glyph_t);
    if (size < table)
        goto bad;

    // checked now so the panic itself never has to
    const bpf_glyph_t *glyphs = (const bpf_glyph_t *)(data + sizeof(*head));
    for (uint32_t i = 0; i < head->count; i++) {
        if ((size_t)glyphs[i].offset + (size_t)glyphs[i].w * glyphs[i].h > size - table)
            goto bad;
    }
    if ('?' < head->first || '?' >= head->first + head->count)
        goto bad;

    font->head   = head;
    font->glyphs = glyphs;
    font->pixels = data + table;
    return true;

bad:
    kfree(data);
    return false;
}

bool panic_screen_load(void) {
    if (framebuffer_get_bpp(0) != 32)
        return false;

    size_t size;
    ps.logo = load_file(LOGO_PATH, &size);
    if (ps.logo && size < LOGO_SIZE * LOGO_SIZE * 4) {
        kfree(ps.logo);
        ps.logo = NULL;
    }

    if (!font_load(&ps.title, TITLE_PATH) || !font_load(&ps.sub, SUB_PATH) || !font_load(&ps.mono, MONO_PATH))
        return false;

    ps.fb      = (uint32_t *)framebuffer_get_addr(0);
    ps.width   = (int)framebuffer_get_width(0);
    ps.height  = (int)framebuffer_get_height(0);
    ps.pitch32 = (int)(framebuffer_get_pitch(0) / 4);
    ps.ready   = ps.fb != NULL;
    return ps.ready;
}

bool panic_screen_ready(void) {
    return ps.ready;
}

// drawing

static void fill(int x, int y, int w, int h, uint32_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ps.width)  w = ps.width - x;
    if (y + h > ps.height) h = ps.height - y;

    for (int row = 0; row < h; row++) {
        uint32_t *line = ps.fb + (size_t)(y + row) * ps.pitch32 + x;
        for (int col = 0; col < w; col++)
            line[col] = colour;
    }
}

// everything lands on black so coverage just scales the colour, vram never gets read back
static uint32_t shade(uint32_t colour, uint32_t a) {
    return (((colour >> 16 & 0xFF) * a / 255) << 16) |
           (((colour >> 8  & 0xFF) * a / 255) << 8)  |
            ((colour       & 0xFF) * a / 255);
}

static const bpf_glyph_t *glyph(const bpf_font_t *font, char c) {
    uint32_t i = (uint8_t)c - font->head->first;
    if (i >= font->head->count)
        i = '?' - font->head->first;
    return &font->glyphs[i];
}

static int line_height(const bpf_font_t *font) {
    return font->head->line * ps.scale;
}

static int text_width(const bpf_font_t *font, const char *s) {
    int w = 0;
    for (; *s; s++)
        w += glyph(font, *s)->advance;
    return w * ps.scale;
}

// y is the top of the line
static void text_draw(const bpf_font_t *font, int x, int y, const char *s, uint32_t colour) {
    int base = y + font->head->ascent * ps.scale;

    for (; *s; s++) {
        const bpf_glyph_t *g  = glyph(font, *s);
        const uint8_t     *px = font->pixels + g->offset;
        int gx = x + g->left * ps.scale;
        int gy = base - g->top * ps.scale;

        for (int row = 0; row < g->h; row++) {
            for (int col = 0; col < g->w; col++) {
                uint8_t a = px[row * g->w + col];
                if (a)
                    fill(gx + col * ps.scale, gy + row * ps.scale, ps.scale, ps.scale, shade(colour, a));
            }
        }
        x += g->advance * ps.scale;
    }
}

static void text_centered(const bpf_font_t *font, int y, const char *s, uint32_t colour) {
    text_draw(font, (ps.width - text_width(font, s)) / 2, y, s, colour);
}

// each pixel is the average of the block of the logo it covers, so it shrinks without going jagged
static void logo_draw(int x0, int y0, int size) {
    for (int y = 0; y < size; y++) {
        int sy0 = y * LOGO_SIZE / size;
        int sy1 = (y + 1) * LOGO_SIZE / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int x = 0; x < size; x++) {
            int sx0 = x * LOGO_SIZE / size;
            int sx1 = (x + 1) * LOGO_SIZE / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    uint32_t c = ((const uint32_t *)ps.logo)[sy * LOGO_SIZE + sx];
                    r += c >> 16 & 0xFF;
                    g += c >> 8  & 0xFF;
                    b += c       & 0xFF;
                    n++;
                }
            }
            fill(x0 + x, y0 + y, 1, 1, (r / n) << 16 | (g / n) << 8 | (b / n));
        }
    }
}

// the details next to the qr code

static int info_collect(info_row_t *rows, struct isr_stackframe *sf, const char *pstring, uint64_t cr2) {
    int n = 0;
    task_t *task = get_current_task();

    rows[n].label = "Cause";
    if (sf)
        snprintf(rows[n].value, sizeof(rows[n].value), "%s (vector 0x%02llx)", exception_name(sf->vector), sf->vector);
    else
        snprintf(rows[n].value, sizeof(rows[n].value), "%s", pstring);
    n++;

    if (sf && sf->vector == 14) {
        page_fault_info_t pf;
        page_fault_describe(sf->error_code, cr2, &pf);

        rows[n].label = "Page fault";
        // the rest of it is in the report, this has to fit next to the code
        snprintf(rows[n].value, sizeof(rows[n].value), "%s at 0x%llx, %s",
                 (sf->error_code & (1 << 4)) ? "EXECUTE" : pf.access, cr2, pf.note ? pf.note : pf.violation);
        n++;
    }

    if (sf) {
        rows[n].label = "Error code";
        snprintf(rows[n].value, sizeof(rows[n].value), "0x%llx", sf->error_code);
        n++;
    }

    rows[n].label = "Task";
    if (task)
        snprintf(rows[n].value, sizeof(rows[n].value), "%s (pid %llu) on cpu %u", task->name, task->id, this_cpu()->id);
    else
        snprintf(rows[n].value, sizeof(rows[n].value), "<none> on cpu %u", this_cpu()->id);
    n++;

    if (sf) {
        uint64_t sym_addr;
        const char *name = stack_trace_symbol_lookup(sf->rip, &sym_addr);

        rows[n].label = "Instruction";
        if (name)
            snprintf(rows[n].value, sizeof(rows[n].value), "0x%016llx <%s+0x%llx>", sf->rip, name, sf->rip - sym_addr);
        else
            snprintf(rows[n].value, sizeof(rows[n].value), "0x%016llx", sf->rip);
        n++;
    }
    return n;
}

void panic_screen_draw(struct isr_stackframe *sf, const char *pstring, const char *report) {
    if (!ps.ready) return;

    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    info_row_t info[INFO_MAX];
    int info_n = info_collect(info, sf, pstring, cr2);

    ps.scale = ps.width >= 2800 && ps.height >= 1800 ? 2 : 1;
    int s      = ps.scale;
    int margin = MARGIN * s;
    fill(0, 0, ps.width, ps.height, 0);

    // low ecc keeps the code small
    bool has_qr = report && qrcodegen_encodeText(report, qr_temp, qr, qrcodegen_Ecc_LOW,
                      qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
    int modules = has_qr ? qrcodegen_getSize(qr) + QR_QUIET * 2 : 0;
    int gap     = has_qr ? QR_GAP * s : 0;
    int head_h  = line_height(&ps.title) + 8 * s + line_height(&ps.sub) * 2 + 32 * s;

    // the code gets whatever height is left and the logo gives way to it, a full report is a big code
    int logo   = ps.logo ? (ps.height >= LOGO_FULL_H * s ? LOGO_SIZE * s : LOGO_SIZE * s / 2) : 0;
    int module = 0;
    for (;;) {
        int logo_h = logo ? logo + 24 * s : 0;
        int room_h = ps.height - margin * 2 - head_h - logo_h;
        int room_w = ps.width / 2;
        int room   = room_h < room_w ? room_h : room_w;

        module = has_qr ? room / modules : 0;
        if (module >= QR_MODULE_MIN * s || !logo) break;
        logo = logo > LOGO_SIZE * s / 2 ? LOGO_SIZE * s / 2 : 0;
    }
    if (module > QR_MODULE_MAX * s) module = QR_MODULE_MAX * s;
    if (has_qr && module < 1) module = 1;
    int qr_w = modules * module;

    int cw          = glyph(&ps.mono, '0')->advance * s;
    int rh          = line_height(&ps.mono) + 8 * s;
    int col_chars   = (ps.width - margin * 2 - qr_w - gap) / cw;
    if (col_chars > COL_CHARS_MAX) col_chars = COL_CHARS_MAX;
    int value_chars = col_chars - LABEL_W;
    if (value_chars < 8) value_chars = 8;

    int text_w = text_width(&ps.sub, PANIC_SCAN);
    for (int i = 0; i < info_n; i++) {
        info[i].value[value_chars] = 0;
        int w = (LABEL_W + (int)strlen(info[i].value)) * cw;
        if (w > text_w) text_w = w;
    }
    int text_h = info_n * rh + 20 * s + line_height(&ps.sub) * 2;

    int block_h = qr_w > text_h ? qr_w : text_h;
    int logo_h  = logo ? logo + 24 * s : 0;
    int y       = (ps.height - logo_h - head_h - block_h) / 2;
    if (y < margin) y = margin;

    if (logo) {
        logo_draw((ps.width - logo) / 2, y, logo);
        y += logo_h;
    }

    text_centered(&ps.title, y, PANIC_TITLE, COL_TITLE);
    y += line_height(&ps.title) + 8 * s;
    text_centered(&ps.sub, y, PANIC_SUB1, COL_DIM);
    y += line_height(&ps.sub);
    text_centered(&ps.sub, y, PANIC_SUB2, COL_DIM);
    y += line_height(&ps.sub) + 32 * s;

    int x = (ps.width - qr_w - gap - text_w) / 2;
    if (x < margin) x = margin;

    // we have to keep it dark on light some phone cameras wont read it any other way
    if (has_qr) {
        int qy   = y + (block_h - qr_w) / 2;
        int size = modules - QR_QUIET * 2;

        fill(x, qy, qr_w, qr_w, COL_QR_LIGHT);
        for (int my = 0; my < size; my++) {
            for (int mx = 0; mx < size; mx++) {
                if (qrcodegen_getModule(qr, mx, my))
                    fill(x + (QR_QUIET + mx) * module, qy + (QR_QUIET + my) * module, module, module, 0);
            }
        }
        x += qr_w + gap;
    }

    int ty   = y + (block_h - text_h) / 2;
    int text = (rh - line_height(&ps.mono)) / 2;
    for (int i = 0; i < info_n; i++) {
        text_draw(&ps.mono, x, ty + text, info[i].label, COL_DIM);
        text_draw(&ps.mono, x + LABEL_W * cw, ty + text, info[i].value, COL_TEXT);
        ty += rh;
    }

    ty += 20 * s;
    text_draw(&ps.sub, x, ty, has_qr ? PANIC_SCAN : PANIC_NO_QR, COL_TITLE);
    text_draw(&ps.sub, x, ty + line_height(&ps.sub), PANIC_SITE, COL_DIM);
}
