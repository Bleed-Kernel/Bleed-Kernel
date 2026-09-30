#include <exec/elf_load.h>
#include <mm/kalloc.h>
#include <string.h>
#include <user/user_copy.h>
#include <user/errno.h>

void exec_args_free(exec_args_t *args) {
    if (!args || !args->argv) return;

    for (int i = 0; i < args->argc; i++) {
        if (args->argv[i])
            kfree(args->argv[i]);
    }
    kfree(args->argv);
    args->argv = NULL;
    args->argc = 0;
}

/// @brief copy a user argv into kernel memory, each string sized to fit
/// @param caller task that owns the argv
/// @param user_argv_ptr user pointer to the argv array
/// @param user_argc number of entries in the argv array
/// @param path used as argv[0] when the caller passes no argv
/// @param out receives the copied arguments, free with exec_args_free
/// @return 0 on success, negative errno on failure
long exec_args_copy_from_user(task_t *caller, uint64_t user_argv_ptr, uint64_t user_argc,
                              const char *path, exec_args_t *out) {
    if (!caller || !out) return -EFAULT;
    out->argc = 0;
    out->argv = NULL;

    int use_user_argv = user_argc > 0 && user_ptr_valid(user_argv_ptr);
    if (use_user_argv && user_argc > EXEC_MAX_ARGS)
        return -E2BIG;

    int argc = use_user_argv ? (int)user_argc : 1;
    out->argv = kmalloc(sizeof(char *) * (size_t)argc);
    if (!out->argv) return -ENOMEM;
    memset(out->argv, 0, sizeof(char *) * (size_t)argc);

    if (!use_user_argv) {
        size_t len = strlen(path);
        out->argv[0] = kmalloc(len + 1);
        if (!out->argv[0]) {
            kfree(out->argv);
            out->argv = NULL;
            return -ENOMEM;
        }
        memcpy(out->argv[0], path, len + 1);
        out->argc = 1;
        return 0;
    }

    size_t budget = EXEC_MAX_ARGV_BYTES;
    for (int i = 0; i < argc; i++) {
        // count as we go so exec_args_free only walks what was filled in
        out->argc = i + 1;

        uint64_t user_arg_ptr = 0;
        if (copy_from_user(caller, &user_arg_ptr,
                           (const void *)(user_argv_ptr + (uint64_t)i * sizeof(uint64_t)),
                           sizeof(user_arg_ptr)) != 0 ||
            !user_ptr_valid(user_arg_ptr)) {
            exec_args_free(out);
            return -EFAULT;
        }

        long len = user_strnlen(caller, (const char *)user_arg_ptr, budget);
        if (len < 0) {
            exec_args_free(out);
            return len == -2 ? -E2BIG : -EFAULT;
        }

        size_t size = (size_t)len + 1;
        out->argv[i] = kmalloc(size);
        if (!out->argv[i]) {
            exec_args_free(out);
            return -ENOMEM;
        }

        if (copy_from_user(caller, out->argv[i], (const void *)user_arg_ptr, size) != 0) {
            exec_args_free(out);
            return -EFAULT;
        }
        out->argv[i][len] = '\0';
        budget -= size;
    }

    return 0;
}
