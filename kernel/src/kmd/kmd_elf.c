#include <kmd/kmd_loader.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>
#include <mm/paging.h>
#include <user/errno.h>
#include <ansii.h>
#include <string.h>

typedef struct {
    const uint8_t    *file;
    size_t            size;
    const ELF64_EHDR *eh;
    const ELF64_Shdr *sh;
    const char       *shstr;
    uint64_t         *sec_addr;
} kmd_elf_t;

// the three runs a driver gets laid out in, each starts on its own page so it can have its own flags
enum { RUN_TEXT, RUN_RODATA, RUN_DATA, RUN_COUNT };

static bool kmd_elf_open(kmd_elf_t *e, const uint8_t *file, size_t size) {
    memset(e, 0, sizeof(*e));
    e->file = file;
    e->size = size;
    e->eh   = (const ELF64_EHDR *)file;

    if (size < sizeof(ELF64_EHDR) || memcmp(e->eh->e_ident, "\x7f" "ELF", 4) != 0)
        return false;
    if (e->eh->e_ident[4] != EI_CLASS64 || e->eh->e_type != ET_REL || e->eh->e_machine != EM_X86_64)
        return false;
    if (e->eh->e_shentsize != sizeof(ELF64_Shdr) || e->eh->e_shstrndx >= e->eh->e_shnum)
        return false;
    if (e->eh->e_shoff + (uint64_t)e->eh->e_shnum * sizeof(ELF64_Shdr) > size)
        return false;

    e->sh = (const ELF64_Shdr *)(file + e->eh->e_shoff);
    for (size_t i = 0; i < e->eh->e_shnum; i++) {
        if (e->sh[i].sh_type != SHT_NOBITS && e->sh[i].sh_offset + e->sh[i].sh_size > size)
            return false;
    }
    e->shstr = (const char *)(file + e->sh[e->eh->e_shstrndx].sh_offset);
    return true;
}

static const ELF64_Shdr *kmd_elf_section(kmd_elf_t *e, const char *name, size_t *index) {
    for (size_t i = 0; i < e->eh->e_shnum; i++) {
        if (strcmp(e->shstr + e->sh[i].sh_name, name) == 0) {
            if (index)
                *index = i;
            return &e->sh[i];
        }
    }
    return NULL;
}

static int kmd_run_of(const ELF64_Shdr *s) {
    if (s->sh_flags & SHF_EXECINSTR)
        return RUN_TEXT;
    if (s->sh_flags & SHF_WRITE)
        return RUN_DATA;
    return RUN_RODATA;
}

void kmd_elf_depends(const uint8_t *file, size_t size, char *out, size_t max) {
    out[0] = 0;

    kmd_elf_t e;
    if (!kmd_elf_open(&e, file, size))
        return;

    const ELF64_Shdr *s = kmd_elf_section(&e, ".kmd_depends", NULL);
    if (!s)
        return;

    size_t len = s->sh_size < max - 1 ? s->sh_size : max - 1;
    memcpy(out, file + s->sh_offset, len);
    out[len] = 0;
}

// the kernel first, then any driver already loaded. a driver it comes from gets a ref
static uint64_t kmd_resolve(kmd_t *kmd, const char *name, bool *found) {
    uint64_t addr = kmd_export_find(name);
    if (addr) {
        *found = true;
        return addr;
    }

    for (kmd_t *other = kmd_first(); other; other = other->next) {
        for (size_t i = 0; i < other->sym_count; i++) {
            ELF64_Sym *s = &other->syms[i];
            if (s->st_shndx == SHN_UNDEF || ELF64_ST_BIND(s->st_info) == STB_LOCAL)
                continue;
            if (strcmp(other->strtab + s->st_name, name) != 0)
                continue;

            bool have = false;
            for (size_t d = 0; d < kmd->dep_count; d++)
                have |= kmd->deps[d] == other;
            if (!have && kmd->dep_count < KMD_DEPS_MAX) {
                kmd->deps[kmd->dep_count++] = other;
                other->refs++;
            }

            *found = true;
            return s->st_value;
        }
    }

    *found = false;
    return 0;
}

static bool kmd_layout(kmd_elf_t *e, size_t *pages, uint64_t run_start[RUN_COUNT], uint64_t run_end[RUN_COUNT]) {
    uint64_t at = 0;

    for (int run = 0; run < RUN_COUNT; run++) {
        at = PAGE_ALIGN_UP(at);
        run_start[run] = at;

        for (size_t i = 0; i < e->eh->e_shnum; i++) {
            const ELF64_Shdr *s = &e->sh[i];
            if (!(s->sh_flags & SHF_ALLOC) || !s->sh_size || kmd_run_of(s) != run)
                continue;

            uint64_t align = s->sh_addralign ? s->sh_addralign : 1;
            if (align > PAGE_SIZE)
                return false;
            at = (at + align - 1) & ~(align - 1);
            e->sec_addr[i] = at;   // an offset for now, the base gets added once theres memory
            at += s->sh_size;
        }
        run_end[run] = at;
    }

    *pages = PAGE_ALIGN_UP(at) / PAGE_SIZE;
    return *pages != 0;
}

static int kmd_symbols(kmd_t *kmd, kmd_elf_t *e) {
    size_t symtab_index = 0;
    const ELF64_Shdr *symtab = NULL;
    for (size_t i = 0; i < e->eh->e_shnum; i++) {
        if (e->sh[i].sh_type == SHT_SYMTAB) {
            symtab       = &e->sh[i];
            symtab_index = i;
            break;
        }
    }
    if (!symtab || symtab->sh_link >= e->eh->e_shnum)
        return -ENOEXEC;

    const ELF64_Shdr *strtab = &e->sh[symtab->sh_link];
    size_t count = symtab->sh_size / sizeof(ELF64_Sym);
    (void)symtab_index;

    kmd->syms   = kmalloc(count * sizeof(ELF64_Sym));
    kmd->strtab = kmalloc(strtab->sh_size + 1);
    if (!kmd->syms || !kmd->strtab)
        return -ENOMEM;

    memcpy(kmd->syms, e->file + symtab->sh_offset, count * sizeof(ELF64_Sym));
    memcpy(kmd->strtab, e->file + strtab->sh_offset, strtab->sh_size);
    kmd->strtab[strtab->sh_size] = 0;
    kmd->sym_count = count;

    int missing = 0;
    for (size_t i = 1; i < count; i++) {
        ELF64_Sym  *s    = &kmd->syms[i];
        const char *name = kmd->strtab + s->st_name;

        if (s->st_shndx == SHN_UNDEF) {
            bool found = false;
            s->st_value = kmd_resolve(kmd, name, &found);
            if (!found && ELF64_ST_BIND(s->st_info) != STB_WEAK) {
                serial_printf(LOG_ERROR "kmd: %s wants '%s' and nothing has it\n", kmd->name, name);
                missing++;
            }
        } else if (s->st_shndx == SHN_COMMON) {
            serial_printf(LOG_ERROR "kmd: %s has a common symbol '%s', build it with -fno-common\n",
                          kmd->name, name);
            missing++;
        } else if (s->st_shndx < e->eh->e_shnum) {
            if (e->sec_addr[s->st_shndx])
                s->st_value += e->sec_addr[s->st_shndx];
        }
    }
    return missing ? -ENOENT : 0;
}

static bool kmd_fits32(int64_t v, bool is_signed) {
    if (is_signed)
        return v >= INT32_MIN && v <= INT32_MAX;
    return (uint64_t)v <= UINT32_MAX;
}

static int kmd_relocate(kmd_t *kmd, kmd_elf_t *e) {
    for (size_t i = 0; i < e->eh->e_shnum; i++) {
        const ELF64_Shdr *rs = &e->sh[i];
        if (rs->sh_type == SHT_REL) {
            serial_printf(LOG_ERROR "kmd: %s has SHT_REL relocations, x86_64 uses RELA\n", kmd->name);
            return -ENOEXEC;
        }
        if (rs->sh_type != SHT_RELA || rs->sh_info >= e->eh->e_shnum)
            continue;

        // relocations against debug info and the like, nothing loads them
        uint64_t target = e->sec_addr[rs->sh_info];
        if (!target)
            continue;

        const ELF64_Rela *rela  = (const ELF64_Rela *)(e->file + rs->sh_offset);
        size_t            count = rs->sh_size / sizeof(ELF64_Rela);

        for (size_t r = 0; r < count; r++) {
            uint64_t sym  = ELF64_R_SYM(rela[r].r_info);
            uint32_t type = ELF64_R_TYPE(rela[r].r_info);
            if (sym >= kmd->sym_count)
                return -ENOEXEC;

            uint64_t P = target + rela[r].r_offset;
            uint64_t S = kmd->syms[sym].st_value;
            int64_t  A = rela[r].r_addend;
            int64_t  v;

            switch (type) {
                case R_X86_64_NONE:
                    break;
                case R_X86_64_64:
                    *(uint64_t *)P = S + A;
                    break;
                case R_X86_64_PC64:
                    *(uint64_t *)P = S + A - P;
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32:
                    v = (int64_t)(S + A - P);
                    if (!kmd_fits32(v, true))
                        goto overflow;
                    *(uint32_t *)P = (uint32_t)v;
                    break;
                case R_X86_64_32S:
                    v = (int64_t)(S + A);
                    if (!kmd_fits32(v, true))
                        goto overflow;
                    *(uint32_t *)P = (uint32_t)v;
                    break;
                case R_X86_64_32:
                    v = (int64_t)(S + A);
                    if (!kmd_fits32(v, false))
                        goto overflow;
                    *(uint32_t *)P = (uint32_t)v;
                    break;
                default:
                    serial_printf(LOG_ERROR "kmd: %s has relocation type %u we dont do\n", kmd->name, type);
                    return -ENOEXEC;
            }
            continue;

        overflow:
            serial_printf(LOG_ERROR "kmd: %s relocation type %u against '%s' doesnt fit (0x%llx from 0x%llx)\n",
                          kmd->name, type, kmd->strtab + kmd->syms[sym].st_name, S + A, P);
            return -ENOEXEC;
        }
    }
    return 0;
}

int kmd_elf_link(kmd_t *kmd, const uint8_t *file, size_t size) {
    kmd_elf_t e;
    if (!kmd_elf_open(&e, file, size)) {
        serial_printf(LOG_ERROR "kmd: %s isnt an x86_64 relocatable elf\n", kmd->name);
        return -ENOEXEC;
    }

    size_t abi_index = 0, drv_index = 0;
    const ELF64_Shdr *abi = kmd_elf_section(&e, ".kmd_abi", &abi_index);
    const ELF64_Shdr *drv = kmd_elf_section(&e, ".kmd", &drv_index);
    if (!abi || abi->sh_size < 8 || !drv || drv->sh_size < sizeof(kmd_driver_t)) {
        serial_printf(LOG_ERROR "kmd: %s has no KMD_DRIVER in it\n", kmd->name);
        return -ENOEXEC;
    }

    const uint32_t *stamp = (const uint32_t *)(file + abi->sh_offset);
    if (stamp[0] != KMD_MAGIC || stamp[1] != KMD_ABI) {
        serial_printf(LOG_ERROR "kmd: %s was built for kmd abi %u, this kernel is %u\n",
                      kmd->name, stamp[1], KMD_ABI);
        return -ENOEXEC;
    }

    // only a warning, a driver from an older sdk usually still fits. KMD_ABI is the hard line
    const ELF64_Shdr *built = kmd_elf_section(&e, ".kmd_kernel", NULL);
    if (built && built->sh_size) {
        char   want[64] = {0};
        size_t len      = built->sh_size < sizeof(want) - 1 ? built->sh_size : sizeof(want) - 1;
        memcpy(want, file + built->sh_offset, len);
        if (kmd_kernel_version[0] && strcmp(want, kmd_kernel_version) != 0)
            serial_printf(LOG_WARN "kmd: %s was built with the sdk from %s, this kernel is %s\n",
                          kmd->name, want, kmd_kernel_version);
    }

    e.sec_addr = kmalloc(e.eh->e_shnum * sizeof(uint64_t));
    if (!e.sec_addr)
        return -ENOMEM;
    memset(e.sec_addr, 0, e.eh->e_shnum * sizeof(uint64_t));

    int      err = -ENOEXEC;
    uint64_t run_start[RUN_COUNT], run_end[RUN_COUNT];
    if (!kmd_layout(&e, &kmd->pages, run_start, run_end))
        goto out;

    // has to be within 2G of the kernel, drivers are built -mcmodel=kernel same as it is
    kmd->base = (uintptr_t)vmm_alloc_near_kernel(kmd->pages);
    if (!kmd->base) {
        err = -ENOMEM;
        goto out;
    }

    for (size_t i = 0; i < e.eh->e_shnum; i++) {
        const ELF64_Shdr *s = &e.sh[i];
        if (!(s->sh_flags & SHF_ALLOC) || !s->sh_size)
            continue;
        e.sec_addr[i] += kmd->base;
        if (s->sh_type != SHT_NOBITS)
            memcpy((void *)e.sec_addr[i], file + s->sh_offset, s->sh_size);
    }

    serial_printf(LOG_INFO "kmd: %s at 0x%llx, %llu pages (text %llu, rodata %llu, data %llu bytes)\n",
                  kmd->name, (uint64_t)kmd->base, (uint64_t)kmd->pages, run_end[RUN_TEXT] - run_start[RUN_TEXT],
                  run_end[RUN_RODATA] - run_start[RUN_RODATA], run_end[RUN_DATA] - run_start[RUN_DATA]);

    if ((err = kmd_symbols(kmd, &e)) < 0 || (err = kmd_relocate(kmd, &e)) < 0)
        goto out;

    // text runs and is read only, rodata is read only, data stays as vmm gave it
    size_t text_pages = (PAGE_ALIGN_UP(run_end[RUN_TEXT]) - run_start[RUN_TEXT]) / PAGE_SIZE;
    size_t ro_pages   = (PAGE_ALIGN_UP(run_end[RUN_RODATA]) - run_start[RUN_RODATA]) / PAGE_SIZE;
    if (text_pages)
        vmm_protect_pages(kernel_page_map, (void *)(kmd->base + run_start[RUN_TEXT]), text_pages, PAGE_KERNEL_RO);
    if (ro_pages)
        vmm_protect_pages(kernel_page_map, (void *)(kmd->base + run_start[RUN_RODATA]), ro_pages,
                          PAGE_KERNEL_RO | PTE_NX);

    kmd->driver = (const kmd_driver_t *)e.sec_addr[drv_index];
    err = 0;

out:
    kfree(e.sec_addr);
    return err;
}
