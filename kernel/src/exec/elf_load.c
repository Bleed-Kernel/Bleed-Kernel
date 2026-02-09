#include <exec/elf.h>
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

#define ELF_MAGIC "\x7F""ELF"

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

        char *load_buffer = kmalloc(segment_bytes);
        if (!load_buffer) { r = -OUT_OF_MEMORY; goto out_phdr; }

        memset(load_buffer, 0, segment_bytes);

        uintptr_t vert_offset = phdr[i].p_vaddr - seg_start;
        if (phdr[i].p_filesz > segment_bytes - vert_offset) { r = -INVALID_MAGIC; goto out_buf; }

        r = vfs_read_exact(elf_file,
                           load_buffer + vert_offset,
                           phdr[i].p_filesz,
                           phdr[i].p_offset);
        if (r != 0) goto out_buf;

        for (uintptr_t off = 0; off < segment_bytes; off += PAGE_SIZE){
            paddr_t phys = pmm_alloc_pages(1);
            if (!phys) { r = -OUT_OF_MEMORY; goto out_buf; }

            paging_map_page(cr3, phys, seg_start + off, pflags);

            size_t copy_size = PAGE_SIZE;
            if (off + copy_size > segment_bytes)
                copy_size = segment_bytes - off;

            memcpy((void*)paddr_to_vaddr(phys),
                   load_buffer + off,
                   copy_size);
        }

        kfree(load_buffer, segment_bytes);
        continue;

out_buf:
        kfree(load_buffer, segment_bytes);
        goto out_phdr;
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
    path_t filepath = vfs_path_from_abs(path);

    vfs_lookup(&filepath, &file);
    return file;
}

task_t *elf_sched(INode_t *file){
    if (!file) return NULL;

    paddr_t cr3 = paging_create_address_space();
    if (!cr3) return NULL;

    uintptr_t entry = 0;
    if (elf_load(file, cr3, &entry) != 0) return NULL;

    task_t *task = sched_create_task(cr3, entry, USER_CS, USER_SS, file->internal_data);
    if (!task) return NULL;

    stac();
    uint64_t *user_stack = (uint64_t *)(USER_STACK_TOP - 16);
    user_stack[0] = 0;
    user_stack[1] = 0;
    clac();

    task->context->rsp = (uint64_t)user_stack;

    task->current_directory = vfs_get_root();
    task->current_directory->shared++;

    return task;
}
