#include <kmd/kmd_loader.h>
#include <drivers/serial/serial.h>
#include <kernel/bootargs.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>
#include <mm/spinlock.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

#ifndef BLEED_VERSION
#define BLEED_VERSION ""
#endif

const char kmd_kernel_version[] = BLEED_VERSION;

static kmd_t        *kmd_list     = NULL;
static kmd_device_t *kmd_devs     = NULL;
static size_t        kmd_dev_count = 0;
static bool          kmd_running  = false;    // past the point start gets called

// drivers that didnt load this boot, so a second driver depending on one doesnt try it all again
static char          kmd_failed[KMD_FAILED_MAX][KMD_NAME_MAX];
static size_t        kmd_failed_count = 0;

static bool kmd_has_failed(const char *name) {
    for (size_t i = 0; i < kmd_failed_count; i++) {
        if (strcmp(kmd_failed[i], name) == 0)
            return true;
    }
    return false;
}

static void kmd_mark_failed(const char *name) {
    if (kmd_failed_count < KMD_FAILED_MAX && !kmd_has_failed(name))
        strncpy(kmd_failed[kmd_failed_count++], name, KMD_NAME_MAX - 1);
}

kmd_t *kmd_first(void) {
    return kmd_list;
}

kmd_t *kmd_find(const char *name) {
    for (kmd_t *k = kmd_list; k; k = k->next) {
        if (strcmp(k->name, name) == 0)
            return k;
    }
    return NULL;
}

size_t kmd_device_count(void) {
    return kmd_dev_count;
}

kmd_device_t *kmd_device_get(size_t index) {
    return index < kmd_dev_count ? &kmd_devs[index] : NULL;
}

static void kmd_devices_init(void) {
    pci_scan();

    kmd_dev_count = pci_count();
    if (!kmd_dev_count)
        return;

    kmd_devs = kmalloc(kmd_dev_count * sizeof(kmd_device_t));
    if (!kmd_devs) {
        kmd_dev_count = 0;
        return;
    }
    memset(kmd_devs, 0, kmd_dev_count * sizeof(kmd_device_t));

    for (size_t i = 0; i < kmd_dev_count; i++) {
        kmd_devs[i].bus = KMD_BUS_PCI;
        kmd_devs[i].pci = *pci_get(i);
    }
}

static bool kmd_field(uint32_t want, uint32_t have) {
    return want == KMD_ANY || want == have;
}

static bool kmd_pci_matches(const kmd_pci_match_t *m, const pci_device_t *p) {
    for (; m->vendor || m->device || m->class_code || m->subclass || m->prog_if; m++) {
        if (kmd_field(m->vendor, p->vendor_id) && kmd_field(m->device, p->device_id) &&
            kmd_field(m->class_code, p->class_code) && kmd_field(m->subclass, p->subclass) &&
            kmd_field(m->prog_if, p->prog_if))
            return true;
    }
    return false;
}

static void kmd_probe(kmd_t *kmd) {
    const kmd_driver_t *drv = kmd->driver;
    if (!drv->probe || !drv->pci)
        return;

    for (size_t i = 0; i < kmd_dev_count; i++) {
        kmd_device_t *dev = &kmd_devs[i];
        if (dev->owner || !kmd_pci_matches(drv->pci, &dev->pci))
            continue;

        int r = drv->probe(dev);
        if (r == 0) {
            dev->owner = kmd;
            kmd->devices++;
            serial_printf(LOG_OK "kmd: %s bound to " PCI_SPEC " %04x:%04x\n", kmd->name,
                          PCI_SPEC_ARGS(&dev->pci), dev->pci.vendor_id, dev->pci.device_id);
        } else {
            dev->priv = NULL;
            serial_printf(LOG_INFO "kmd: %s passed on " PCI_SPEC " %04x:%04x (%d)\n", kmd->name,
                          PCI_SPEC_ARGS(&dev->pci), dev->pci.vendor_id, dev->pci.device_id, r);
        }
    }
}

static void kmd_free(kmd_t *kmd) {
    for (size_t d = 0; d < kmd->dep_count; d++)
        kmd->deps[d]->refs--;
    if (kmd->base)
        vmm_free_pages((void *)kmd->base, kmd->pages);
    kfree(kmd->syms);
    kfree(kmd->strtab);
    kfree(kmd);
}

static uint8_t *kmd_read_file(const char *path, size_t *size) {
    INode_t *file = NULL;
    path_t   p    = vfs_path_from_abs(path);
    if (vfs_lookup(&p, &file) < 0 || !file)
        return NULL;

    *size = vfs_filesize(file);
    uint8_t *buf = *size ? kmalloc(*size) : NULL;
    if (buf && inode_read(file, buf, *size, 0) != (long)*size) {
        kfree(buf);
        buf = NULL;
    }
    vfs_drop(file);
    return buf;
}

static bool kmd_skipped(const char *name) {
    const char *skip = bootargs_get("kmd-skip");
    if (!skip)
        return false;

    size_t len = strlen(name);
    for (const char *p = skip; *p; ) {
        const char *end = p;
        while (*end && *end != ',')
            end++;
        if ((size_t)(end - p) == len && strncmp(p, name, len) == 0)
            return true;
        p = *end ? end + 1 : end;
    }
    return false;
}

static kmd_t *kmd_load(const char *name, int depth);

static bool kmd_load_depends(const char *name, const char *depends, int depth) {
    char want[KMD_NAME_MAX];
    for (const char *p = depends; *p; ) {
        while (*p == ' ' || *p == ',')
            p++;
        size_t len = 0;
        while (p[len] && p[len] != ' ' && p[len] != ',')
            len++;
        if (!len)
            break;

        if (len >= sizeof(want)) {
            serial_printf(LOG_ERROR "kmd: %s depends on a name thats too long\n", name);
            return false;
        }
        memcpy(want, p, len);
        want[len] = 0;
        p += len;

        if (!kmd_load(want, depth + 1)) {
            serial_printf(LOG_INFO "kmd: %s not loaded, it needs %s\n", name, want);
            return false;
        }
    }
    return true;
}

static kmd_t *kmd_load_one(const char *name, int depth);

static kmd_t *kmd_load(const char *name, int depth) {
    kmd_t *kmd = kmd_find(name);
    if (kmd || kmd_has_failed(name))
        return kmd;

    kmd = kmd_load_one(name, depth);
    if (!kmd)
        kmd_mark_failed(name);
    return kmd;
}

static kmd_t *kmd_load_one(const char *name, int depth) {
    kmd_t *kmd = NULL;

    if (depth > KMD_DEPTH_MAX) {
        serial_printf(LOG_ERROR "kmd: %s is part of a depends loop\n", name);
        return NULL;
    }
    if (strlen(name) >= KMD_NAME_MAX) {
        serial_printf(LOG_ERROR "kmd: name '%s' is too long\n", name);
        return NULL;
    }
    if (kmd_skipped(name)) {
        serial_printf(LOG_WARN "kmd: %s skipped, its in kmd-skip\n", name);
        return NULL;
    }

    char path[96];
    snprintf(path, sizeof(path), KMD_DIR "/%s.KMD", name);

    size_t   size = 0;
    uint8_t *file = kmd_read_file(path, &size);
    if (!file) {
        serial_printf(LOG_ERROR "kmd: cant read %s\n", path);
        return NULL;
    }

    char depends[128];
    kmd_elf_depends(file, size, depends, sizeof(depends));
    if (depends[0] && !kmd_load_depends(name, depends, depth)) {
        kfree(file);
        return NULL;
    }

    kmd = kmalloc(sizeof(kmd_t));
    if (!kmd) {
        kfree(file);
        return NULL;
    }
    memset(kmd, 0, sizeof(*kmd));
    strncpy(kmd->name, name, sizeof(kmd->name) - 1);

    int err = kmd_elf_link(kmd, file, size);
    kfree(file);
    if (err < 0) {
        serial_printf(LOG_ERROR "kmd: %s didnt link (%d)\n", name, err);
        kmd_free(kmd);
        return NULL;
    }

    const kmd_driver_t *drv = kmd->driver;
    // no hardware for it is the usual reason, not something to shout about
    if (drv->load && (err = drv->load()) != 0) {
        if (err == -ENODEV)
            serial_printf(LOG_INFO "kmd: %s found nothing to drive, not keeping it\n", name);
        else
            serial_printf(LOG_WARN "kmd: %s load said no (%d), throwing it away\n", name, err);
        irq_drop_range(kmd->base, kmd->base + kmd->pages * PAGE_SIZE);
        kmd_free(kmd);
        return NULL;
    }

    // on the end so the list stays in load order, unload walks dependants before what they need
    kmd_t **tail = &kmd_list;
    while (*tail)
        tail = &(*tail)->next;
    *tail = kmd;

    serial_printf(LOG_OK "kmd: %s loaded%s%s\n", name, drv->description ? ", " : "",
                  drv->description ? drv->description : "");

    kmd_probe(kmd);

    if (kmd_running && drv->start) {
        drv->start();
        kmd->started = true;
    }
    return kmd;
}

// one name a line, # for comments
static void kmd_boot_conf(char *conf) {
    for (char *line = conf; *line; ) {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        char next = *end;
        *end = 0;

        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r'))
            line[--len] = 0;

        if (len)
            kmd_load(line, 0);

        line = next ? end + 1 : end;
    }
}

// no drivers.conf, every .KMD in the folder in whatever order it lists them
static void kmd_boot_dir(void) {
    INode_t *dir = NULL;
    path_t   p   = vfs_path_from_abs(KMD_DIR);
    if (vfs_lookup(&p, &dir) < 0 || !dir) {
        serial_printf(LOG_WARN "kmd: no " KMD_DIR ", no drivers\n");
        return;
    }

    for (size_t i = 0; ; i++) {
        INode_t *child = NULL;
        if (vfs_readdir(dir, i, &child) < 0 || !child)
            break;

        char   name[KMD_NAME_MAX];
        size_t len = strlen(child->name);
        if (len > 4 && len - 4 < sizeof(name) && strcmp(child->name + len - 4, ".KMD") == 0) {
            memcpy(name, child->name, len - 4);
            name[len - 4] = 0;
            kmd_load(name, 0);
        }
        vfs_drop(child);
    }
    vfs_drop(dir);
}

size_t kmd_boot(void) {
    kmd_devices_init();

    if (bootargs_has("no-kmd")) {
        serial_printf(LOG_WARN "kmd: no-kmd given, not loading any drivers\n");
        return 0;
    }

    if (!kmd_exports_load(KMD_EXPORTS)) {
        serial_printf(LOG_ERROR "kmd: no " KMD_EXPORTS ", drivers cant be linked\n");
        return 0;
    }

    size_t size = 0;
    char  *conf = (char *)kmd_read_file(KMD_CONF, &size);
    if (conf) {
        char *text = kmalloc(size + 1);
        if (text) {
            memcpy(text, conf, size);
            text[size] = 0;
            kmd_boot_conf(text);
            kfree(text);
        }
        kfree(conf);
    } else {
        kmd_boot_dir();
    }

    size_t loaded = 0;
    for (kmd_t *k = kmd_list; k; k = k->next)
        loaded++;
    return loaded;
}

void kmd_start_all(void) {
    kmd_running = true;
    for (kmd_t *k = kmd_list; k; k = k->next) {
        if (k->started || !k->driver->start)
            continue;
        k->driver->start();
        k->started = true;
    }
}

int kmd_unload(const char *name) {
    kmd_t *kmd = kmd_find(name);
    if (!kmd)
        return -ENOENT;

    const kmd_driver_t *drv = kmd->driver;
    if (kmd->refs) {
        serial_printf(LOG_WARN "kmd: %s is still used by %d other drivers\n", name, kmd->refs);
        return -EBUSY;
    }
    if (!drv->unload) {
        serial_printf(LOG_WARN "kmd: %s has no unload, it stays\n", name);
        return -EPERM;
    }

    for (size_t i = 0; i < kmd_dev_count; i++) {
        kmd_device_t *dev = &kmd_devs[i];
        if (dev->owner != kmd)
            continue;

        int r = drv->remove ? drv->remove(dev) : 0;
        if (r != 0) {
            serial_printf(LOG_WARN "kmd: %s wont let go of " PCI_SPEC " (%d)\n", name, PCI_SPEC_ARGS(&dev->pci), r);
            return -EBUSY;
        }
        dev->owner = NULL;
        dev->priv  = NULL;
        kmd->devices--;
    }

    int r = drv->unload();
    if (r != 0) {
        serial_printf(LOG_WARN "kmd: %s unload said no (%d)\n", name, r);
        return -EBUSY;
    }

    // anything it forgot to unregister would jump into freed memory on the next tick
    size_t left = irq_drop_range(kmd->base, kmd->base + kmd->pages * PAGE_SIZE);
    if (left)
        serial_printf(LOG_WARN "kmd: %s left %llu irq/tick handlers behind, dropped them\n", name, (uint64_t)left);

    for (kmd_t **pp = &kmd_list; *pp; pp = &(*pp)->next) {
        if (*pp == kmd) {
            *pp = kmd->next;
            break;
        }
    }

    serial_printf(LOG_OK "kmd: %s unloaded\n", name);
    kmd_free(kmd);
    return 0;
}

const char *kmd_symbol_lookup(uint64_t addr, uint64_t *sym_addr, const char **module) {
    for (kmd_t *k = kmd_list; k; k = k->next) {
        if (addr < k->base || addr >= k->base + k->pages * PAGE_SIZE)
            continue;

        const char *name = NULL;
        uint64_t    best = 0;
        for (size_t i = 0; i < k->sym_count; i++) {
            ELF64_Sym *s = &k->syms[i];
            if (ELF64_ST_TYPE(s->st_info) != STT_FUNC || s->st_shndx == SHN_UNDEF)
                continue;
            if (s->st_value <= addr && s->st_value >= best) {
                best = s->st_value;
                name = k->strtab + s->st_name;
            }
        }
        if (module)
            *module = k->name;
        if (sym_addr)
            *sym_addr = best;
        return name;
    }
    return NULL;
}

size_t kmd_describe(char *text, size_t max) {
    size_t at = 0;
    for (kmd_t *k = kmd_list; k && at < max; k = k->next) {
        int n = snprintf(text + at, max - at, "%-16s %4llu KiB  %llu devices  %d users%s\n", k->name,
                         (uint64_t)(k->pages * PAGE_SIZE / 1024), (uint64_t)k->devices, k->refs,
                         k->driver->unload ? "" : "  (stays)");
        if (n < 0)
            break;
        at += (size_t)n;
    }
    return at < max ? at : max;
}
