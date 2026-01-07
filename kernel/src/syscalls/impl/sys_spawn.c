#include <stdint.h>
#include <mm/kalloc.h>
#include <sched/scheduler.h>
#include <fs/vfs.h>
#include <drivers/serial/serial.h>
#include <user/user_copy.h>
#include <exec/elf_load.h>
#include <gdt/gdt.h>
#include <mm/paging.h>
#include <ansii.h>

uint64_t sys_spawn(uint64_t user_path_ptr) {
    char *kpath = user_copy_string((const char *)user_path_ptr, 256);
    if (!kpath) return -1;

    INode_t *file = NULL;
    path_t filepath = vfs_path_from_abs(kpath);
    vfs_lookup(&filepath, &file);
    if (!file) return -1;

    paddr_t cr3 = paging_create_address_space();
    uintptr_t entry;

    int elf_load_ret = elf_load(file, cr3, &entry);
    if (elf_load_ret < 0)
        return elf_load_ret;
    
    task_t *child = sched_create_task(cr3, entry, USER_CS, USER_SS);
    child->wait_queue = NULL;
    child->state = TASK_READY;

    serial_printf("%sNew Task Created: PID %d\n", LOG_INFO, child->id);

    return child->id;
}
