#include <exec/elf.h>
#include <stdint.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <fs/vfs.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <sched/scheduler.h>
#include <gdt/gdt.h>
#include <stdio.h>
#include <ansii.h>
#include <string.h>
#include <status.h>

#define ELF_MAGIC "\x7F""ELF"

int elf_load(INode_t *elf_file, paddr_t cr3, uintptr_t* entry){
    ELF64_EHDR ehdr = {0};
    long r = vfs_read_exact(elf_file, &ehdr, sizeof(ehdr), 0);
    if (r != 0) return r;

    if (memcmp(ELF_MAGIC, ehdr.e_ident, 4) != 0){
        kprintf(LOG_ERROR "This does not look like an elf file\n");
        return -INVALID_MAGIC;
    }

    if (ehdr.e_type != ET_EXEC){
        kprintf(LOG_ERROR "ELF file is not of Executable Type\n");
        return -INVALID_MAGIC;
    }

    if (ehdr.e_ident[4] != EI_CLASS64){
        kprintf(LOG_ERROR "This is not a 64-bit Executable, Required by Bleed\n");
        return -INVALID_MAGIC;
    }

    if (ehdr.e_ident[5] != EI_LITTLE_ENDIAN){
        kprintf(LOG_ERROR "Data is not encoded as Little Endian, Required by Bleed\n");
        return -INVALID_MAGIC;
    }

    if (ehdr.e_phentsize != sizeof(ELF64_Phdr)){
        return -INVALID_MAGIC;
    }

    size_t phdr_size = (ehdr.e_phentsize * ehdr.e_phnum);
    ELF64_Phdr *phdr = kmalloc(phdr_size);
    if (!phdr) return -OUT_OF_MEMORY;
    r = vfs_read_exact(elf_file, phdr, phdr_size, ehdr.e_phoff);
    if (r != 0) goto read_phdr;

    for (int i = 0; i < ehdr.e_phnum; i++){
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_flags != 0){
            uint64_t pflags = PTE_USER | PTE_PRESENT;
            if (phdr[i].p_flags & PF_W) pflags |= PTE_WRITABLE;

            uintptr_t segment_bytes = (PAGE_ALIGN_UP(phdr[i].p_vaddr + phdr[i].p_memsz)) - PAGE_ALIGN_DOWN(phdr[i].p_vaddr);
            uintptr_t vert_segment_start = PAGE_ALIGN_DOWN(phdr[i].p_vaddr);
            uintptr_t vert_offset = phdr[i].p_vaddr - vert_segment_start;

            char *load_buffer = kmalloc(segment_bytes);

            if (!load_buffer) {
                r = -OUT_OF_MEMORY;
                goto read_phdr;
            }

            memset(load_buffer, 0, segment_bytes);

            if (phdr[i].p_filesz > segment_bytes - vert_offset){
                r = -INVALID_MAGIC;
                kfree(load_buffer, segment_bytes);
                goto read_phdr;
            }

            r = vfs_read_exact(elf_file, load_buffer + vert_offset, phdr[i].p_filesz, phdr[i].p_offset);
            if (r < 0){
                kfree(load_buffer, segment_bytes);
                goto read_phdr;
            }

            for (size_t j = 0; j < segment_bytes / PAGE_SIZE; j++){
                size_t off = j * PAGE_SIZE;

                // allocate a page for user-space
                paddr_t phys = pmm_alloc_pages(1);
                if (!phys) {
                    r = -OUT_OF_MEMORY;
                    kfree(load_buffer, segment_bytes);
                    goto read_phdr;
                }

                // map page into task page table
                paging_map_page(cr3, phys, vert_segment_start + off, pflags);

                // copy contents from load_buffer into newly mapped page
                size_t copy_size = PAGE_SIZE;
                if (off == 0 && vert_offset != 0)
                    copy_size -= vert_offset;

                memcpy((void*)paddr_to_vaddr(phys) + (off == 0 ? vert_offset : 0),
                       load_buffer + off,
                       copy_size);
            }

            kfree(load_buffer, segment_bytes);
        }
    }

    *entry = ehdr.e_entry;

read_phdr:
    kfree(phdr, phdr_size);
    return r;
}

// helpers

INode_t *elf_get_from_path(const char *path){
    INode_t *file = NULL;
    path_t filepath = vfs_path_from_abs(path);

    vfs_lookup(&filepath, &file);
    if (file)
        return file;
    else
        return NULL;
}

task_t *elf_sched(INode_t *file){
    paddr_t cr3 = paging_create_address_space();

    uintptr_t entry;
    if (file != NULL) elf_load(file, cr3, &entry);

    task_t *task = sched_create_task(cr3, entry, USER_CS, USER_SS);
    task->heap_base = 0x400000;
    task->heap_end  = task->heap_base;

    uint64_t *user_stack = (uint64_t *)(USER_STACK_TOP - USER_STACK_SIZE + USER_STACK_SIZE - 16);
    user_stack[0] = 0;
    user_stack[1] = 0;

    task->context->rsp = (uint64_t)user_stack;

    task->current_directory = vfs_get_root();
    task->current_directory->shared++;
    return task;
}
