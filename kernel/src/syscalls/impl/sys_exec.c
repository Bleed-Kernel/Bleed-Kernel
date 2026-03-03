#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <exec/elf_load.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/kalloc.h>
#include <mm/userspace/mmap.h>
#include <user/errno.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

static void free_user_alloc_nodes(user_alloc_t *alloc) {
    while (alloc) {
        user_alloc_t *next = alloc->next;
        kfree(alloc, sizeof(user_alloc_t));
        alloc = next;
    }
}

static const char *exec_display_name(const char *path, const char *inode_name) {
    if (inode_name && inode_name[0] != '\0')
        return inode_name;

    if (!path || path[0] == '\0')
        return "Bleed Program";

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }

    return (*base) ? base : "Bleed Program";
}

long sys_exec(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc) {
    task_t *task = get_current_task();

    if (!user_path_ptr || !user_ptr_valid(user_path_ptr)) {
        return -EFAULT;
    }

    if (!task || task->task_privilege != P_USER) {
        return -ESRCH;
    }

    char kpath[EXEC_MAX_ARG_LEN];
    for (size_t i = 0; i < sizeof(kpath); i++) kpath[i] = 0;
    if (copy_user_string(task, (const char *)user_path_ptr, kpath, sizeof(kpath)) != 0) {
        return -EFAULT;
    }

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

    int parse_error = 0;
    if (use_extended) {
        for (uint64_t i = 0; i < user_argc; i++) {
            uint64_t user_arg_ptr = 0;
            if (copy_from_user(task,
                               &user_arg_ptr,
                               (const void *)(user_argv_ptr + i * sizeof(uint64_t)),
                               sizeof(user_arg_ptr)) != 0)
                parse_error = 1;

            if (!parse_error && !user_ptr_valid(user_arg_ptr))
                parse_error = 1;

            if (parse_error)
                break;

            argv_owned[i] = kmalloc(EXEC_MAX_ARG_LEN);
            if (!argv_owned[i]) {
                for (size_t j = 0; j < EXEC_MAX_ARGS; j++) {
                    if (argv_owned[j])
                        kfree(argv_owned[j], EXEC_MAX_ARG_LEN);
                }
                return -ENOMEM;
            }

            if (copy_user_string(task,
                                 (const char *)user_arg_ptr,
                                 argv_owned[i],
                                 EXEC_MAX_ARG_LEN) != 0) {
                for (size_t j = 0; j < EXEC_MAX_ARGS; j++) {
                    if (argv_owned[j])
                        kfree(argv_owned[j], EXEC_MAX_ARG_LEN);
                }
                return -EFAULT;
            }

            argv_kernel[i] = argv_owned[i];
        }
        if (!parse_error)
            argc = (int)user_argc;
    }
    if (parse_error) {
        for (size_t i = 0; i < EXEC_MAX_ARGS; i++) {
            if (argv_owned[i])
                kfree(argv_owned[i], EXEC_MAX_ARG_LEN);
        }
        return -EFAULT;
    }

    long ret = -EIO;
    paddr_t new_cr3 = 0;
    paddr_t old_cr3 = task->page_map;
    user_alloc_t *old_alloc_list = task->alloc_list;
    user_heap_t *old_heap = task->heap;
    user_heap_t *new_heap = NULL;
    INode_t *file = elf_get_from_path(kpath);

    if (!file) {
        ret = -ENOENT;
        goto done;
    }

    new_cr3 = paging_create_address_space();
    if (!new_cr3) {
        ret = -ENOMEM;
        goto done;
    }

    uintptr_t entry = 0;
    if (elf_load(file, new_cr3, &entry) != 0) {
        ret = -ENOEXEC;
        goto fail_new_cr3;
    }

    for (uint64_t page = USER_STACK_TOP - USER_STACK_SIZE; page < USER_STACK_TOP; page += PAGE_SIZE) {
        paddr_t paddr = pmm_alloc_pages(1);
        if (!paddr) {
            ret = -ENOMEM;
            goto fail_new_cr3;
        }
        paging_map_page_invl(new_cr3, paddr, page, PTE_USER | PTE_WRITABLE, 0);
    }

    new_heap = kmalloc(sizeof(user_heap_t));
    if (!new_heap) {
        ret = -ENOMEM;
        goto fail_new_cr3;
    }
    new_heap->task = task;
    new_heap->current = 0x0000004000000000ULL;
    new_heap->end = new_heap->current;

    task->page_map = new_cr3;
    task->alloc_list = NULL;
    task->heap = new_heap;
    task->sig_active_frame = 0;

    cpu_context_t *ctx = task->context;
    if (!ctx) {
        ret = -EIO;
        goto rollback_task;
    }

    ctx->rip = entry;
    ctx->rsp = USER_STACK_TOP;
    ctx->rflags |= 0x200ULL;
    ctx->rax = 0;

    if (elf_setup_user_args(task, argc, argv_kernel) != 0) {
        ret = -EFAULT;
        goto rollback_task;
    }

    paging_switch_address_space(new_cr3);

    const char *new_name = exec_display_name(kpath, file->internal_data);
    strncpy(task->name, new_name, sizeof(task->name) - 1);
    task->name[sizeof(task->name) - 1] = '\0';

    free_user_alloc_nodes(old_alloc_list);
    if (old_heap)
        kfree(old_heap, sizeof(user_heap_t));
    paging_destroy_address_space(old_cr3);

    serial_printf(LOG_OK "exec success pid=%u path=%s\n",
                  (unsigned)task->id,
                  kpath);

    ret = 0;
    goto done;

rollback_task:
    task->page_map = old_cr3;
    task->alloc_list = old_alloc_list;
    task->heap = old_heap;
    if (new_heap) {
        kfree(new_heap, sizeof(user_heap_t));
        new_heap = NULL;
    }
fail_new_cr3:
    if (new_cr3)
        paging_destroy_address_space(new_cr3);
done:
    for (size_t i = 0; i < EXEC_MAX_ARGS; i++) {
        if (argv_owned[i])
            kfree(argv_owned[i], EXEC_MAX_ARG_LEN);
    }
    if (ret < 0) {
        serial_printf("[PROC] exec failed pid=%u err=%d path=%s\n",
                      (unsigned)task->id,
                      (int)ret,
                      kpath);
    }
    return ret;
}
