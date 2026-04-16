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
#include <status.h>
#include <user/user_copy.h>
#include <syscalls/syscall.h>

#define ELF_MAGIC "\x7F""ELF"

static const uint8_t exec_exit_trampoline[] = {
    0x89, 0xC7,                                 // preserve main() return code
    0xB8, SYS_EXIT, 0x00, 0x00, 0x00,
    0x0F, 0x05,
    0x0F, 0x0B
};

int elf_setup_user_args(task_t *task, int argc, const char *const argv[]) {
    if (!task || argc < 0 || argc > EXEC_MAX_ARGS) return -1;
    if (argc > 0 && !argv) return -1;

    uint64_t argv_user[EXEC_MAX_ARGS + 1];
    uintptr_t tramp_addr = (USER_STACK_TOP - sizeof(exec_exit_trampoline)) & ~0xFULL;
    uintptr_t sp = tramp_addr;
    uintptr_t stack_floor = USER_STACK_TOP - USER_STACK_SIZE;

    if (tramp_addr < stack_floor) return -1;
    if (copy_to_user(task, (void *)tramp_addr, exec_exit_trampoline, sizeof(exec_exit_trampoline)) != 0) return -1;

    for (int i = argc - 1; i >= 0; i--) {
        if (!argv[i]) return -1;

        size_t len = strlen(argv[i]);
        if (len == EXEC_MAX_ARG_LEN) return -1;
        len += 1;

        if (sp < len) return -1;
        sp -= len;
        sp &= ~0x7ULL;

        if (sp < stack_floor) return -1;
        if (copy_to_user(task, (void *)sp, argv[i], len) != 0) return -1;

        argv_user[i] = (uint64_t)sp;
    }

    argv_user[argc] = 0;

    size_t argv_bytes = (size_t)(argc + 1) * sizeof(uint64_t);
    size_t frame_bytes = sizeof(uint64_t) + argv_bytes;
    if (sp < frame_bytes) return -1;

    uintptr_t frame_base = (sp - frame_bytes) & ~0xFULL;
    uintptr_t user_argv = frame_base + sizeof(uint64_t);
    if (frame_base < stack_floor) return -1;
    if (user_argv + argv_bytes > sp) return -1;

    uint64_t ret_addr = (uint64_t)tramp_addr;
    if (copy_to_user(task, (void *)frame_base, &ret_addr, sizeof(ret_addr)) != 0) return -1;
    if (copy_to_user(task, (void *)user_argv, argv_user, argv_bytes) != 0) return -1;

    sp = frame_base;

    task->context->rsp = (uint64_t)sp;
    task->context->rdi = (uint64_t)argc;
    task->context->rsi = (uint64_t)user_argv;

    return 0;
}

int elf_load(INode_t *elf_file, paddr_t cr3, uintptr_t* entry){
    if (!elf_file || !entry) return -INVALID_MAGIC;

    ELF64_EHDR ehdr;
    memset(&ehdr, 0, sizeof(ehdr));

    long r = vfs_read_exact(elf_file, &ehdr, sizeof(ehdr), 0);
    if (r != 0) return r;

    if (memcmp(ELF_MAGIC, ehdr.e_ident, 4) != 0)    return -INVALID_MAGIC;
    if (ehdr.e_type != ET_EXEC)                     return -INVALID_MAGIC;
    if (ehdr.e_ident[4] != EI_CLASS64)              return -INVALID_MAGIC;
    if (ehdr.e_ident[5] != EI_LITTLE_ENDIAN)        return -INVALID_MAGIC;
    if (ehdr.e_phentsize != sizeof(ELF64_Phdr))     return -INVALID_MAGIC;
    if (ehdr.e_phnum == 0)                          return -INVALID_MAGIC;
    if (ehdr.e_phoff < sizeof(ELF64_EHDR))          return -INVALID_MAGIC;

    size_t phdr_size = ehdr.e_phentsize * ehdr.e_phnum;
    if (phdr_size / ehdr.e_phentsize != ehdr.e_phnum) return -INVALID_MAGIC;

    ELF64_Phdr *phdr = kmalloc(phdr_size);
    if (!phdr) return -OUT_OF_MEMORY;

    r = vfs_read_exact(elf_file, phdr, phdr_size, ehdr.e_phoff);
    if (r != 0) goto out_phdr;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++){
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_memsz == 0) continue;

        if (phdr[i].p_memsz < 0 ||
            phdr[i].p_filesz > (ELF64_LONG)phdr[i].p_memsz) {
            r = -INVALID_MAGIC;
            goto out_phdr;
        }

        uint64_t pflags = PTE_USER | PTE_PRESENT;
        if (phdr[i].p_flags & PF_W) pflags |= PTE_WRITABLE;

        uintptr_t seg_start = PAGE_ALIGN_DOWN(phdr[i].p_vaddr);
        uintptr_t seg_end;
        if (__builtin_add_overflow(phdr[i].p_vaddr, phdr[i].p_memsz, &seg_end)) { r = -INVALID_MAGIC; goto out_phdr; }
        seg_end = PAGE_ALIGN_UP(seg_end);

        uintptr_t segment_bytes = seg_end - seg_start;
        if (segment_bytes == 0) continue;

        for (uintptr_t off = 0; off < segment_bytes; off += PAGE_SIZE) {
            paddr_t phys = pmm_alloc_pages(1);
            if (!phys) { r = -OUT_OF_MEMORY; goto out_phdr; }

            paging_map_page_invl(cr3, phys, seg_start + off, pflags, 0);

            void *kvaddr = (void*)paddr_to_vaddr(phys);
            memset(kvaddr, 0, PAGE_SIZE);

            uintptr_t current_page_vaddr = seg_start + off;
            uintptr_t file_data_start = phdr[i].p_vaddr;
            uintptr_t file_data_end = phdr[i].p_vaddr + phdr[i].p_filesz;

            if (current_page_vaddr < file_data_end && (current_page_vaddr + PAGE_SIZE) > file_data_start) {
                uintptr_t read_start_in_page = 0;
                uintptr_t file_offset = phdr[i].p_offset;

                if (current_page_vaddr < file_data_start) {
                    read_start_in_page = file_data_start - current_page_vaddr;
                } else {
                    file_offset += (current_page_vaddr - file_data_start);
                }

                uintptr_t read_end_in_page = PAGE_SIZE;
                if ((current_page_vaddr + PAGE_SIZE) > file_data_end) {
                    read_end_in_page = file_data_end - current_page_vaddr;
                }

                size_t read_size = read_end_in_page - read_start_in_page;

                r = vfs_read_exact(elf_file, 
                                   (char*)kvaddr + read_start_in_page, 
                                   read_size, 
                                   file_offset);
                                   
                if (r != 0) goto out_phdr;
            }
        }
        
        continue;
    }

    *entry = ehdr.e_entry;
    r = 0;

out_phdr:
    kfree(phdr, phdr_size);
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
    const char *argv_default[1] = { (file->internal_data) ? file->internal_data : "program" };

    if (argc <= 0 || !argv) {
        argc = 1;
        argv = argv_default;
    }

    paddr_t cr3 = paging_create_address_space();
    if (!cr3) return NULL;

    uintptr_t entry = 0;
    if (elf_load(file, cr3, &entry) != 0) return NULL;

    task_t *task = sched_create_task(cr3, entry, USER_CS, USER_SS, file->internal_data);
    if (!task) return NULL;
    if (elf_setup_user_args(task, argc, argv) != 0) {
        sched_mark_task_dead(task);
        return NULL;
    }

    return task;
}