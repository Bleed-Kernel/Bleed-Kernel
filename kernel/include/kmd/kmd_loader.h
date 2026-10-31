#pragma once

#include <kmd/kmd.h>
#include <exec/elf.h>

#define KMD_EXPORTS         "/initrd/etc/kernel.exports"
#define KMD_DEPS_MAX        8
#define KMD_DEPTH_MAX       8       // a depends chain longer than this is a loop
#define KMD_TEXT_MAX        2048    // the most /dev/kmd will say
#define KMD_FAILED_MAX      32

typedef struct kmd {
    char                name[KMD_NAME_MAX];
    const kmd_driver_t *driver;

    uintptr_t           base;       // all of its sections, text then rodata then data
    size_t              pages;

    ELF64_Sym          *syms;       // st_value already turned into the real address
    size_t              sym_count;
    char               *strtab;

    struct kmd         *deps[KMD_DEPS_MAX];
    size_t              dep_count;
    int                 refs;       // drivers linked against us
    size_t              devices;    // how many it has bound
    bool                started;
    struct kmd         *next;
} kmd_t;

// the build this kernel is, drivers carry the one their sdk was made from
extern const char kmd_kernel_version[];

// load and probe whatever drivers.conf lists, or every .KMD there is without one. how many loaded
size_t kmd_boot(void);

// call start on every driver, the scheduler has to be running
void kmd_start_all(void);

// remove its devices, unload it and free it, 0 or a negative errno
int kmd_unload(const char *name);

kmd_t *kmd_find(const char *name);
kmd_t *kmd_first(void);

// the driver symbol nearest below addr for stack traces, module gets the drivers name
const char *kmd_symbol_lookup(uint64_t addr, uint64_t *sym_addr, const char **module);

// a text listing of whats loaded, what /dev/kmd reads back
size_t kmd_describe(char *text, size_t max);

// kmd_elf.c, link a relocatable elf into kmd
int kmd_elf_link(kmd_t *kmd, const uint8_t *file, size_t size);

// the raw KMD_DEPENDS string
void kmd_elf_depends(const uint8_t *file, size_t size, char *out, size_t max);

// kmd_exports.c
bool     kmd_exports_load(const char *path);
uint64_t kmd_export_find(const char *name);

// kmd_device.c, /dev/kmd
void kmd_device_init(void);
