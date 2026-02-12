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

uint64_t sys_spawn(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc) {
    if (!user_path_ptr) return -1;
    if (!user_ptr_valid(user_path_ptr)) return -2;

    char kpath[EXEC_MAX_ARG_LEN];
    for (size_t i = 0; i < sizeof(kpath); i++) kpath[i] = 0;

    task_t *caller = get_current_task();
    if (copy_user_string(caller, (const char *)user_path_ptr, kpath, sizeof(kpath)) != 0)
        return -3;

    INode_t *file = elf_get_from_path(kpath);
    if (!file) return -4;
    task_t *child = NULL;

    const char *argv_kernel[EXEC_MAX_ARGS];
    char *argv_owned[EXEC_MAX_ARGS];
    for (size_t i = 0; i < EXEC_MAX_ARGS; i++) {
        argv_kernel[i] = NULL;
        argv_owned[i] = NULL;
    }

    int argc = 1;
    argv_kernel[0] = kpath;

    int use_extended = user_argc > 0 &&
                       user_argc <= EXEC_MAX_ARGS &&
                       user_ptr_valid(user_argv_ptr);

    if (use_extended) {
        for (uint64_t i = 0; i < user_argc; i++) {
            uint64_t user_arg_ptr = 0;
            if (copy_from_user(caller,
                               &user_arg_ptr,
                               (const void *)(user_argv_ptr + i * sizeof(uint64_t)),
                               sizeof(user_arg_ptr)) != 0) {
                goto cleanup;
            }

            if (!user_ptr_valid(user_arg_ptr)) goto cleanup;

            argv_owned[i] = kmalloc(EXEC_MAX_ARG_LEN);
            if (!argv_owned[i]) goto cleanup;

            if (copy_user_string(caller,
                                 (const char *)user_arg_ptr,
                                 argv_owned[i],
                                 EXEC_MAX_ARG_LEN) != 0) {
                goto cleanup;
            }

            argv_kernel[i] = argv_owned[i];
        }

        argc = (int)user_argc;
    }

    child = elf_sched(file, argc, argv_kernel);
    if (!child) goto cleanup;

    child->wait_queue = NULL;
    child->state = TASK_READY;

    serial_printf("%sNew Task Created: PID %d\n", LOG_INFO, child->id);

cleanup:
    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
        if (argv_owned[i])
            kfree(argv_owned[i], EXEC_MAX_ARG_LEN);
    }
    return child ? child->id : (uint64_t)-5;
}
