#include <kmd/kmd_loader.h>
#include <drivers/serial/serial.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <string.h>

// same layout mksymtab writes
typedef struct {
    uint64_t addr;
    uint32_t name_off;
} kexport_t;

static size_t     export_count = 0;
static kexport_t *exports      = NULL;
static const char *export_names = NULL;

static uint32_t *export_table = NULL;
static size_t    export_slots = 0;

static uint32_t export_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

bool kmd_exports_load(const char *path) {
    INode_t *file = NULL;
    path_t   p    = vfs_path_from_abs(path);
    if (vfs_lookup(&p, &file) < 0 || !file)
        return false;

    size_t sz = vfs_filesize(file);
    if (sz < sizeof(size_t)) {
        vfs_drop(file);
        return false;
    }

    uint8_t *buf = kmalloc(sz);
    if (!buf) {
        vfs_drop(file);
        return false;
    }
    if (inode_read(file, buf, sz, 0) != (long)sz) {
        vfs_drop(file);
        kfree(buf);
        return false;
    }
    vfs_drop(file);

    size_t count = *(size_t *)buf;
    if (count > (sz - sizeof(size_t)) / sizeof(kexport_t)) {
        kfree(buf);
        return false;
    }

    size_t slots = 64;
    while (slots < count * 2)
        slots *= 2;

    uint32_t *table = kmalloc(slots * sizeof(uint32_t));
    if (!table) {
        kfree(buf);
        return false;
    }
    memset(table, 0, slots * sizeof(uint32_t));

    exports      = (kexport_t *)(buf + sizeof(size_t));
    export_names = (const char *)(exports + count);
    export_count = count;

    for (size_t i = 0; i < count; i++) {
        size_t slot = export_hash(export_names + exports[i].name_off) & (slots - 1);
        while (table[slot])
            slot = (slot + 1) & (slots - 1);
        table[slot] = (uint32_t)i + 1;
    }
    export_table = table;
    export_slots = slots;

    serial_printf(LOG_INFO "kmd: %llu kernel symbols exported to drivers\n", (uint64_t)count);
    return true;
}

uint64_t kmd_export_find(const char *name) {
    if (!export_table)
        return 0;

    size_t slot = export_hash(name) & (export_slots - 1);
    while (export_table[slot]) {
        kexport_t *e = &exports[export_table[slot] - 1];
        if (strcmp(export_names + e->name_off, name) == 0)
            return e->addr;
        slot = (slot + 1) & (export_slots - 1);
    }
    return 0;
}
