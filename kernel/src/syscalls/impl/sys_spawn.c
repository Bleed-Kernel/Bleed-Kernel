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
#include <user/errno.h>

uint64_t sys_spawn(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc) {
    if (!user_path_ptr)
        return (uint64_t)-EFAULT;
    if (!user_ptr_valid(user_path_ptr))
        return (uint64_t)-EFAULT;

    char kpath[EXEC_MAX_PATH_LEN];
    for (size_t i = 0; i < sizeof(kpath); i++) kpath[i] = 0;

    task_t *caller = get_current_task();
    if (!caller)
        return (uint64_t)-ESRCH;

    uint64_t err = (uint64_t)-EIO;
    if (copy_user_string(caller, (const char *)user_path_ptr, kpath, sizeof(kpath)) != 0)
        return (uint64_t)-EFAULT;

    INode_t *file = elf_get_from_path(kpath);
    if (!file)
        return (uint64_t)-ENOENT;
    task_t *child = NULL;

    exec_args_t args;
    long args_err = exec_args_copy_from_user(caller, user_argv_ptr, user_argc, kpath, &args);
    if (args_err < 0)
        return (uint64_t)args_err;

    child = elf_sched(file, args.argc, (const char *const *)args.argv);
    if (!child) {
        err = (uint64_t)-EIO;
        goto cleanup;
    }

    child->wait_queue = NULL;
    child->state = TASK_READY;

    INode_t *parent_cwd = caller->current_directory ? caller->current_directory : vfs_get_root();
    if (child->current_directory)
        vfs_drop(child->current_directory);
    child->current_directory = parent_cwd;
    if (child->current_directory)
        child->current_directory->shared++;

    serial_printf("%sNew Task Created: PID %d\n", LOG_INFO, child->id);

cleanup:
    exec_args_free(&args);
    return child ? child->id : err;
}
