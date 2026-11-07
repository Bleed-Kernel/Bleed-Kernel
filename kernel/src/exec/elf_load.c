#include <exec/elf.h>
#include <exec/elf_load.h>
#include <stdint.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <sched/scheduler.h>
#include <mm/smap.h>
#include <gdt/gdt.h>
#include <stdio.h>
#include <ansii.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <syscalls/syscall.h>
#include <random/random.h>

#define ELF_MAGIC "\x7F""ELF"

static const uint8_t exec_exit_trampoline[] = {
    0x89, 0xC7,                                 // preserve main() return code
    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0x0F, 0x05,
    0x0F, 0x0B
};

// what kernel started tasks and old style exec callers get
static const char *const default_env[] = { "PATH=/usr/local/bin:/bin", "HOME=/root" };

// strings are packed downward from sp,
static int push_user_strings(task_t *task, uintptr_t *sp, uintptr_t stack_floor,
                             int count, const char *const strs[], uint64_t *out) {
    for (int i = count - 1; i >= 0; i--) {
        if (!strs[i]) return -1;

        size_t len = strlen(strs[i]) + 1;
        if (*sp - stack_floor < len) return -1;
        *sp = (*sp - len) & ~0x7ULL;

        if (*sp < stack_floor) return -1;
        if (copy_to_user(task, (void *)*sp, strs[i], len) != 0) return -1;

        out[i] = (uint64_t)*sp;
    }

    out[count] = 0;
    return 0;
}

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

#define AUXV_MAX    20

int elf_setup_user_args(task_t *task, int argc, const char *const argv[],
                        int envc, const char *const envp[], const elf_image_t *image) {
    if (!task || argc < 0 || argc > EXEC_MAX_ARGS) return -1;
    if (argc > 0 && !argv) return -1;
    if (!envp) {
        envp = default_env;
        envc = sizeof(default_env) / sizeof(default_env[0]);
    }
    if (envc < 0 || envc > EXEC_MAX_ENV) return -1;

    uintptr_t tramp_addr = (USER_STACK_TOP - sizeof(exec_exit_trampoline)) & ~0xFULL;
    uintptr_t sp = tramp_addr;
    uintptr_t stack_floor = USER_STACK_TOP - USER_STACK_SIZE;

    if (tramp_addr < stack_floor) return -1;
    if (copy_to_user(task, (void *)tramp_addr, exec_exit_trampoline, sizeof(exec_exit_trampoline)) != 0) return -1;

    uint8_t rnd[16];
    random_get_bytes(rnd, sizeof(rnd));
    sp = (sp - sizeof(rnd)) & ~0xFULL;
    uintptr_t rnd_addr = sp;
    if (copy_to_user(task, (void *)rnd_addr, rnd, sizeof(rnd)) != 0) return -1;

    size_t argv_bytes  = (size_t)(argc + 1) * sizeof(uint64_t);
    size_t env_bytes   = (size_t)(envc + 1) * sizeof(uint64_t);
    size_t aux_bytes   = AUXV_MAX * 2 * sizeof(uint64_t);
    size_t table_bytes = sizeof(uint64_t) + argv_bytes + env_bytes + aux_bytes;
    uint64_t *table = kmalloc(table_bytes);
    if (!table) return -1;
    memset(table, 0, table_bytes);

    uint64_t *targv = table + 1;
    uint64_t *tenv  = targv + argc + 1;
    uint64_t *taux  = tenv + envc + 1;

    if (push_user_strings(task, &sp, stack_floor, envc, envp, tenv) != 0) goto fail;
    if (push_user_strings(task, &sp, stack_floor, argc, argv, targv) != 0) goto fail;
    table[0] = (uint64_t)argc;

    int n = 0;
#define AUX(k, v) do { taux[n * 2] = (k); taux[n * 2 + 1] = (uint64_t)(v); n++; } while (0)
    if (image && image->phdr) {
        AUX(AT_PHDR, image->phdr);
        AUX(AT_PHENT, sizeof(ELF64_Phdr));
        AUX(AT_PHNUM, image->phnum);
    }
    AUX(AT_PAGESZ, PAGE_SIZE);
    if (image && image->interp_base)
        AUX(AT_BASE, image->interp_base);
    AUX(AT_FLAGS, 0);
    if (image)
        AUX(AT_ENTRY, image->prog_entry);
    AUX(AT_UID, 0);
    AUX(AT_EUID, 0);
    AUX(AT_GID, 0);
    AUX(AT_EGID, 0);
    AUX(AT_HWCAP, 0);
    AUX(AT_CLKTCK, 100);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, rnd_addr);
    if (argc > 0)
        AUX(AT_EXECFN, targv[0]);
    AUX(AT_NULL, 0);
#undef AUX
    size_t used = sizeof(uint64_t) + argv_bytes + env_bytes + (size_t)n * 2 * sizeof(uint64_t);

    if (sp - stack_floor < used + 16) goto fail;
    uintptr_t frame = (sp - used) & ~0xFULL;
    if (copy_to_user(task, (void *)frame, table, used) != 0) goto fail;
    kfree(table);

    task->context->rsp = (uint64_t)frame;
    task->context->rdi = (uint64_t)argc;
    task->context->rsi = (uint64_t)(frame + sizeof(uint64_t));
    task->context->rdx = (uint64_t)(frame + sizeof(uint64_t) + argv_bytes);
    return 0;

fail:
    kfree(table);
    return -1;
}

// one image with its load bias, ET_EXEC sits where it says and ET_DYN at base
static int load_image(INode_t *elf_file, paddr_t cr3, uintptr_t base, int allow_interp,
                      elf_image_t *image, char *interp, size_t interp_size) {
    ELF64_EHDR ehdr;
    memset(&ehdr, 0, sizeof(ehdr));

    long r = vfs_read_exact(elf_file, &ehdr, sizeof(ehdr), 0);
    if (r != 0) return r;

    if (memcmp(ELF_MAGIC, ehdr.e_ident, 4) != 0)    return -ENOEXEC;
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) return -ENOEXEC;
    if (ehdr.e_ident[4] != EI_CLASS64)              return -ENOEXEC;
    if (ehdr.e_ident[5] != EI_LITTLE_ENDIAN)        return -ENOEXEC;
    if (ehdr.e_phentsize != sizeof(ELF64_Phdr))     return -ENOEXEC;
    if (ehdr.e_phnum == 0)                          return -ENOEXEC;
    if (ehdr.e_phoff < sizeof(ELF64_EHDR))          return -ENOEXEC;

    size_t phdr_size = ehdr.e_phentsize * ehdr.e_phnum;
    if (phdr_size / ehdr.e_phentsize != ehdr.e_phnum) return -ENOEXEC;

    ELF64_Phdr *phdr = kmalloc(phdr_size);
    if (!phdr) return -ENOMEM;

    r = vfs_read_exact(elf_file, phdr, phdr_size, ehdr.e_phoff);
    if (r != 0) goto out_phdr;

    // the lowest page of the image lands on base
    uintptr_t bias = 0;
    if (ehdr.e_type == ET_DYN) {
        uintptr_t low = (uintptr_t)-1;
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            if (phdr[i].p_type == PT_LOAD && PAGE_ALIGN_DOWN(phdr[i].p_vaddr) < low)
                low = PAGE_ALIGN_DOWN(phdr[i].p_vaddr);
        }
        if (low == (uintptr_t)-1) { r = -ENOEXEC; goto out_phdr; }
        bias = base - low;
    }

    image->phdr  = 0;
    image->phnum = ehdr.e_phnum;
    if (interp)
        interp[0] = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type == PT_PHDR)
            image->phdr = phdr[i].p_vaddr + bias;
        if (phdr[i].p_type == PT_LOAD && !image->phdr && phdr[i].p_offset <= ehdr.e_phoff &&
            ehdr.e_phoff + phdr_size <= phdr[i].p_offset + phdr[i].p_filesz)
            image->phdr = phdr[i].p_vaddr + (ehdr.e_phoff - phdr[i].p_offset) + bias;
        if (phdr[i].p_type == PT_INTERP) {
            if (!allow_interp || !interp || phdr[i].p_filesz == 0 || phdr[i].p_filesz >= interp_size) {
                r = -ENOEXEC;
                goto out_phdr;
            }
            r = vfs_read_exact(elf_file, interp, phdr[i].p_filesz, phdr[i].p_offset);
            if (r != 0) goto out_phdr;
            interp[phdr[i].p_filesz] = 0;
        }
    }

    for (uint16_t i = 0; i < ehdr.e_phnum; i++){
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_memsz == 0) continue;

        if (phdr[i].p_memsz < 0 ||
            phdr[i].p_filesz > (ELF64_LONG)phdr[i].p_memsz) {
            r = -ENOEXEC;
            goto out_phdr;
        }

        uint64_t pflags = PTE_USER | PTE_PRESENT;
        if (phdr[i].p_flags & PF_W) pflags |= PTE_WRITABLE;
        if (!(phdr[i].p_flags & PF_X)) pflags |= PTE_NX;

        uintptr_t vaddr = phdr[i].p_vaddr + bias;
        uintptr_t seg_start = PAGE_ALIGN_DOWN(vaddr);
        uintptr_t seg_end;
        if (__builtin_add_overflow(vaddr, phdr[i].p_memsz, &seg_end)) { r = -ENOEXEC; goto out_phdr; }
        seg_end = PAGE_ALIGN_UP(seg_end);

        // a segment aimed at the higher half would get mapped straight over the kernel,
        // those tables are shared by every address space
        if (!user_ptr_valid(seg_start) || seg_end <= seg_start || !user_ptr_valid(seg_end - 1)) {
            r = -ENOEXEC;
            goto out_phdr;
        }

        uintptr_t segment_bytes = seg_end - seg_start;
        if (segment_bytes == 0) continue;

        char *load_buffer = kmalloc(segment_bytes);
        if (!load_buffer) { r = -ENOMEM; goto out_phdr; }

        memset(load_buffer, 0, segment_bytes);

        uintptr_t vert_offset = vaddr - seg_start;
        if (phdr[i].p_filesz > segment_bytes - vert_offset) { r = -ENOEXEC; goto out_buf; }

        r = vfs_read_exact(elf_file,
                           load_buffer + vert_offset,
                           phdr[i].p_filesz,
                           phdr[i].p_offset);
        if (r != 0) goto out_buf;

        uintptr_t data_start = vaddr;
        uintptr_t data_end   = vaddr + phdr[i].p_memsz;

        for (uintptr_t off = 0; off < segment_bytes; off += PAGE_SIZE){
            uintptr_t page = seg_start + off;

            // segments are allowed to share a page, mapping a fresh frame over it would
            // throw away what the earlier segment put there so only copy our own bytes in
            uint64_t *pte = paging_get_page(cr3, page, 0);
            if (pte && (*pte & PTE_PRESENT)) {
                uintptr_t lo = data_start > page ? data_start : page;
                uintptr_t hi = data_end < page + PAGE_SIZE ? data_end : page + PAGE_SIZE;

                memcpy((uint8_t *)paddr_to_vaddr(*pte & PADDR_ENTRY_MASK) + (lo - page),
                       load_buffer + (lo - seg_start),
                       hi - lo);
                *pte |= pflags & PTE_WRITABLE;
                if (!(pflags & PTE_NX))
                    *pte &= ~PTE_NX;
                continue;
            }

            paddr_t phys = pmm_alloc_pages(1);
            if (!phys) { r = -ENOMEM; goto out_buf; }

            paging_map_page_invl(cr3, phys, page, pflags, 0);

            // segment_bytes is page aligned so this is always a whole page
            memcpy((void*)paddr_to_vaddr(phys),
                   load_buffer + off,
                   PAGE_SIZE);
        }

        kfree(load_buffer);
        continue;

out_buf:
        kfree(load_buffer);
        goto out_phdr;
    }

    // returning to a non canonical rip faults in ring 0, not in the task
    if (!user_ptr_valid(ehdr.e_entry + bias)) {
        r = -ENOEXEC;
        goto out_phdr;
    }

    image->entry = ehdr.e_entry + bias;
    r = 0;

out_phdr:
    kfree(phdr);
    return r;
}

int elf_load(INode_t *elf_file, paddr_t cr3, elf_image_t *image) {
    if (!elf_file || !image) return -ENOEXEC;
    memset(image, 0, sizeof(*image));

    char interp[EXEC_MAX_PATH_LEN];
    int r = load_image(elf_file, cr3, ELF_DYN_BASE, 1, image, interp, sizeof(interp));
    if (r != 0) return r;
    image->prog_entry = image->entry;
    if (!interp[0])
        return 0;

    // the dynamic linker goes in too and starts first
    INode_t *ld = elf_get_from_path(interp);
    if (!ld) return -ENOENT;

    elf_image_t ld_image;
    memset(&ld_image, 0, sizeof(ld_image));
    r = load_image(ld, cr3, ELF_INTERP_BASE, 0, &ld_image, NULL, 0);
    vfs_drop(ld);
    if (r != 0) return r;

    image->entry       = ld_image.entry;
    image->interp_base = ELF_INTERP_BASE;
    return 0;
}

INode_t *elf_get_from_path(const char *path){
    if (!path) return NULL;

    INode_t *file = NULL;
    task_t *task = get_current_task();
    INode_t *cwd = task ? task->current_directory : NULL;
    path_t filepath = vfs_path_from_relative(path, cwd);

    vfs_lookup(&filepath, &file);
    return file;
}

task_t *elf_sched_held(INode_t *file, int argc, const char *const argv[]){
    if (!file) return NULL;
    const char *name = file->name[0] ? file->name : "program";
    const char *argv_default[1] = { name };

    if (argc <= 0 || !argv) {
        argc = 1;
        argv = argv_default;
    }

    paddr_t cr3 = paging_create_address_space();
    if (!cr3) return NULL;

    // a half loaded image still owns its frames, hand them back
    elf_image_t image;
    if (elf_load(file, cr3, &image) != 0) {
        paging_destroy_address_space(cr3);
        return NULL;
    }

    // held until its stack and registers are in, it cant run on half of them
    task_t *task = sched_create_task_held(cr3, image.entry, USER_CS, USER_SS, name);
    if (!task) return NULL;
    if (vfs_inode_path(file, task->exec_path, sizeof(task->exec_path)) != 0)
        task->exec_path[0] = '\0';
    if (elf_setup_user_args(task, argc, argv, 0, NULL, &image) != 0) {
        sched_mark_task_dead(task);
        return NULL;
    }

    return task;
}

task_t *elf_sched(INode_t *file, int argc, const char *const argv[]){
    task_t *task = elf_sched_held(file, argc, argv);
    if (task)
        sched_wake_task(task);
    return task;
}
