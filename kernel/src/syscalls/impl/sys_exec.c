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
#include <string.h>
#include <cpu/features/fpu.h>
#include <cpu/msrs.h>

static const char *exec_display_name(const char *path, const char *inode_name) {
    if (inode_name && inode_name[0] != '\0')
        return inode_name;

    if (!path || path[0] == '\0')
        return "Unknown Program";

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }

    return (*base) ? base : "Unknown Program";
}

// has_env 0 is the old 3 argument exec, which gets the default environment
static long do_exec(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc,
                    int has_env, uint64_t user_envp_ptr) {
    task_t *task = get_current_task();

    if (!user_path_ptr || !user_ptr_valid(user_path_ptr)) {
        return -EFAULT;
    }

    if (!task || task->task_privilege != P_USER) {
        return -ESRCH;
    }

    char kpath[EXEC_MAX_PATH_LEN];
    memset(kpath, 0, sizeof(kpath));
    if (copy_user_string(task, (const char *)user_path_ptr, kpath, sizeof(kpath)) != 0) {
        return -EFAULT;
    }

    exec_args_t args;
    long args_err = exec_args_copy_from_user(task, user_argv_ptr, user_argc, kpath, &args);
    if (args_err < 0)
        return args_err;

    if (has_env) {
        args_err = exec_env_copy_from_user(task, user_envp_ptr, &args);
        if (args_err < 0) {
            exec_args_free(&args);
            return args_err;
        }
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

    new_heap = kmalloc(sizeof(user_heap_t));
    if (!new_heap) {
        ret = -ENOMEM;
        goto fail_new_cr3;
    }
    new_heap->task = task;
    new_heap->current = USER_MMAP_BASE;
    new_heap->end = new_heap->current;

    task->page_map = new_cr3;
    task->alloc_list = NULL;
    task->heap = new_heap;

    // everything the rollback cant put back is saved first, a failed exec has to
    // return into the old image exactly as it was
    cpu_context_t *ctx = task->context;
    if (!ctx) {
        ret = -EIO;
        goto rollback_task;
    }
    cpu_context_t old_ctx = *ctx;

    ctx->rip = entry;
    ctx->rsp = USER_STACK_TOP;
    ctx->rflags |= 0x200ULL;
    ctx->rax = 0;

    if (elf_setup_user_args(task, args.argc, (const char *const *)args.argv,
                            args.envc, (const char *const *)args.envp) != 0) {
        *ctx = old_ctx;
        ret = -EFAULT;
        goto rollback_task;
    }

    // past the point of no return, the old handlers point into an image that is about to go
    task->sig_pending = 0;
    task->sig_blocked = 0;
    task->sig_active_frame = 0;
    memset(task->sig_handlers, 0, sizeof(task->sig_handlers));
    memset(task->sig_masks, 0, sizeof(task->sig_masks));
    memset(task->sig_flags, 0, sizeof(task->sig_flags));
    memset(task->sig_restorers, 0, sizeof(task->sig_restorers));
    fpu_task_reset(task);
    task->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);

    paging_switch_address_space(new_cr3);

    const char *new_name = exec_display_name(kpath, file->internal_data);
    strncpy(task->name, new_name, sizeof(task->name) - 1);
    task->name[sizeof(task->name) - 1] = '\0';
    if (vfs_inode_path(file, task->exec_path, sizeof(task->exec_path)) != 0)
        task->exec_path[0] = '\0';

    sched_free_alloc_list(old_alloc_list);
    if (old_heap)
        kfree(old_heap);
    paging_destroy_address_space(old_cr3);

    ret = 0;
    goto done;

rollback_task:
    task->page_map = old_cr3;
    task->alloc_list = old_alloc_list;
    task->heap = old_heap;
    if (new_heap) {
        kfree(new_heap);
        new_heap = NULL;
    }
fail_new_cr3:
    if (new_cr3)
        paging_destroy_address_space(new_cr3);
done:
    // elf_get_from_path took a ref for us
    vfs_drop(file);
    exec_args_free(&args);
    if (ret < 0) {
        serial_printf(LOG_ERROR "exec failed pid=%u err=%d path=%s\n",
                      (unsigned)task->id,
                      (int)ret,
                      kpath);
    }
    return ret;
}

long sys_exec(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc) {
    return do_exec(user_path_ptr, user_argv_ptr, user_argc, 0, 0);
}

long sys_execve(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc, uint64_t user_envp_ptr) {
    return do_exec(user_path_ptr, user_argv_ptr, user_argc, 1, user_envp_ptr);
}
