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
    if (!user_path_ptr) return -1;

    char kpath[256];
    for (size_t i = 0; i < sizeof(kpath); i++) kpath[i] = 0;

    task_t *caller = get_current_task();
    if (copy_from_user(caller, kpath, (const char *)user_path_ptr, sizeof(kpath)) != 0)
        return -1;

    INode_t *file = elf_get_from_path(kpath);
    if (!file) return -1;

    task_t *child = elf_sched(file);
    if (!child) return -1;

    child->wait_queue = NULL;
    child->state = TASK_READY;

    serial_printf("%sNew Task Created: PID %d\n", LOG_INFO, child->id);

    return child->id;
}
