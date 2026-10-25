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

#define ELF_MAGIC "\x7F""ELF"

static const uint8_t exec_exit_trampoline[] = {
    0x89, 0xC7,                                 // preserve main() return code
    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0x0F, 0x05,
    0x0F, 0x0B
};

// what kernel started tasks and old style exec callers get
static const char *const default_env[] = { "PATH=/initrd/bin", "HOME=/" };

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

int elf_setup_user_args(task_t *task, int argc, const char *const argv[],
                        int envc, const char *const envp[]) {
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

    // argv then envp, each NULL terminated, the same as linux minus the auxv
    size_t argv_bytes = (size_t)(argc + 1) * sizeof(uint64_t);
    size_t table_bytes = argv_bytes + (size_t)(envc + 1) * sizeof(uint64_t);
    uint64_t *table = kmalloc(table_bytes);
    if (!table) return -1;

    if (push_user_strings(task, &sp, stack_floor, envc, envp, table + argc + 1) != 0) goto fail;
    if (push_user_strings(task, &sp, stack_floor, argc, argv, table) != 0) goto fail;

    size_t frame_bytes = sizeof(uint64_t) + table_bytes;
    if (sp - stack_floor < frame_bytes) goto fail;

    uintptr_t frame_base = (sp - frame_bytes) & ~0xFULL;
    uintptr_t user_argv = frame_base + sizeof(uint64_t);
    if (frame_base < stack_floor) goto fail;
    if (user_argv + table_bytes > sp) goto fail;

    uint64_t ret_addr = (uint64_t)tramp_addr;
    if (copy_to_user(task, (void *)frame_base, &ret_addr, sizeof(ret_addr)) != 0) goto fail;
    if (copy_to_user(task, (void *)user_argv, table, table_bytes) != 0) goto fail;
    kfree(table);

    sp = frame_base;

    task->context->rsp = (uint64_t)sp;
    task->context->rdi = (uint64_t)argc;
    task->context->rsi = (uint64_t)user_argv;
    task->context->rdx = (uint64_t)(user_argv + argv_bytes);

    return 0;

fail:
    kfree(table);
    return -1;
}

int elf_load(INode_t *elf_file, paddr_t cr3, uintptr_t* entry){
    if (!elf_file || !entry) return -ENOEXEC;

    ELF64_EHDR ehdr;
    memset(&ehdr, 0, sizeof(ehdr));

    long r = vfs_read_exact(elf_file, &ehdr, sizeof(ehdr), 0);
    if (r != 0) return r;

    if (memcmp(ELF_MAGIC, ehdr.e_ident, 4) != 0)    return -ENOEXEC;
    if (ehdr.e_type != ET_EXEC)                     return -ENOEXEC;
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

        uintptr_t seg_start = PAGE_ALIGN_DOWN(phdr[i].p_vaddr);
        uintptr_t seg_end;
        if (__builtin_add_overflow(phdr[i].p_vaddr, phdr[i].p_memsz, &seg_end)) { r = -ENOEXEC; goto out_phdr; }
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

        uintptr_t vert_offset = phdr[i].p_vaddr - seg_start;
        if (phdr[i].p_filesz > segment_bytes - vert_offset) { r = -ENOEXEC; goto out_buf; }

        r = vfs_read_exact(elf_file,
                           load_buffer + vert_offset,
                           phdr[i].p_filesz,
                           phdr[i].p_offset);
        if (r != 0) goto out_buf;

        uintptr_t data_start = phdr[i].p_vaddr;
        uintptr_t data_end   = phdr[i].p_vaddr + phdr[i].p_memsz;

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
    if (!user_ptr_valid(ehdr.e_entry)) {
        r = -ENOEXEC;
        goto out_phdr;
    }

    *entry = ehdr.e_entry;
    r = 0;

out_phdr:
    kfree(phdr);
    return r;
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

task_t *elf_sched(INode_t *file, int argc, const char *const argv[]){
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
    uintptr_t entry = 0;
    if (elf_load(file, cr3, &entry) != 0) {
        paging_destroy_address_space(cr3);
        return NULL;
    }

    task_t *task = sched_create_task(cr3, entry, USER_CS, USER_SS, name);
    if (!task) return NULL;
    if (vfs_inode_path(file, task->exec_path, sizeof(task->exec_path)) != 0)
        task->exec_path[0] = '\0';
    if (elf_setup_user_args(task, argc, argv, 0, NULL) != 0) {
        sched_mark_task_dead(task);
        return NULL;
    }

    return task;
}
