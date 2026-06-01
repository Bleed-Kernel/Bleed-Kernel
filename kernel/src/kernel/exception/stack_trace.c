#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <mm/smap.h>
#include <fs/vfs.h>
#include <boot/bootlogger/bootlogger.h>

struct ksym{
    uint64_t addr;
    uint32_t name_off;
};

struct ksymtab{
    size_t count;
    struct ksym *syms;
    const char *strtab;
};

static struct ksymtab kernel_symbols;
static void *kernel_symbols_blob = NULL;
static size_t kernel_symbols_blob_size = 0;

bool stack_trace_load_symbols(const char *path){
    INode_t *file = NULL;
    path_t filepath = vfs_path_from_abs(path);
    if (vfs_lookup(&filepath, &file) < 0 || !file)
        return false;

    size_t sz = vfs_filesize(file);
    if (sz < sizeof(size_t)) {
        vfs_drop(file);
        return false;
    }

    void *buf = kmalloc(sz);
    if (!buf)
        return false;

    if (inode_read(file, buf, sz, 0) != (long)sz) {
        vfs_drop(file);
        kfree(buf);
        return false;
    }
    vfs_drop(file);

    uint8_t *p = buf;
    kernel_symbols.count = *(size_t*)p;
    p += sizeof(size_t);

    if (kernel_symbols.count > (sz / sizeof(struct ksym))) {
        kfree(buf);
        return false;
    }

    size_t syms_bytes = sizeof(struct ksym) * kernel_symbols.count;
    if (sizeof(size_t) + syms_bytes > sz) {
        kfree(buf);
        return false;
    }

    if (kernel_symbols_blob && kernel_symbols_blob_size)
        kfree(kernel_symbols_blob);

    kernel_symbols.syms = (struct ksym *)p;
    p += syms_bytes;

    kernel_symbols.strtab = (const char *)p;
    kernel_symbols_blob = buf;
    kernel_symbols_blob_size = sz;

    return true;
}

const char *stack_trace_symbol_lookup(uint64_t address, uint64_t *sym_addr_out){
    const char *name = NULL;
    uint64_t c = 0;

    for (size_t i = 0; i < kernel_symbols.count; i++){
        uint64_t saddr = kernel_symbols.syms[i].addr;

        if (address >= saddr && saddr >= c){
            c = saddr;
            name = kernel_symbols.strtab + kernel_symbols.syms[i].name_off;
        }
    }

    if (sym_addr_out)
        *sym_addr_out = c;

    return name;
}

void stack_trace_print(uint64_t *rbp) {
    for (int i = 0; i < 16 && rbp; i++) {
        if ((uint64_t)rbp < 0x1000 || ((uint64_t)rbp & 0xF)) break;

        SMAP_ALLOW{
            uint64_t *next_rbp = (uint64_t *)rbp[0];
            uint64_t rip = rbp[1];
            if (!rip) break;
            uint64_t sym_addr = 0;
            const char *name = stack_trace_symbol_lookup(rip, &sym_addr);

            bset_color(BCOL_GREY, BCOL_BLACK);
            bprintf("  ");
            
            bset_color(BCOL_WHITE, BCOL_BLACK);
            bprintf("%p ", (void *)rip);

            bset_color(BCOL_YELLOW, BCOL_BLACK);
            if (name) {
                bprintf("<%s+0x%llu>\n", name, rip - sym_addr);
            } else {
                bprintf("<??:?>\n");
            }
            
            breset_color();

            if (!next_rbp || next_rbp <= rbp || ((uint64_t)next_rbp & 0xF))
                break;
            rbp = next_rbp;
        }
    }
}