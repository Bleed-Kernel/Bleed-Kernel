#include <exec/elf_load.h>
#include <mm/kalloc.h>
#include <string.h>
#include <user/user_copy.h>
#include <user/errno.h>

static void free_strings(char **list, int count) {
    if (!list) return;

    for (int i = 0; i < count; i++) {
        if (list[i])
            kfree(list[i]);
    }
    kfree(list);
}

void exec_args_free(exec_args_t *args) {
    if (!args) return;

    free_strings(args->argv, args->argc);
    free_strings(args->envp, args->envc);
    args->argv = NULL;
    args->argc = 0;
    args->envp = NULL;
    args->envc = 0;
}

static long copy_user_slot(task_t *caller, uint64_t user_array, int index, size_t *budget, char **out) {
    uint64_t user_str = 0;
    *out = NULL;

    if (copy_from_user(caller, &user_str,
                       (const void *)(user_array + (uint64_t)index * sizeof(uint64_t)),
                       sizeof(user_str)) != 0)
        return -EFAULT;
    if (!user_str)
        return 0;
    if (!user_ptr_valid(user_str))
        return -EFAULT;

    long len = user_strnlen(caller, (const char *)user_str, *budget);
    if (len < 0)
        return len == -2 ? -E2BIG : -EFAULT;

    size_t size = (size_t)len + 1;
    char *s = kmalloc(size);
    if (!s)
        return -ENOMEM;

    if (copy_from_user(caller, s, (const void *)user_str, size) != 0) {
        kfree(s);
        return -EFAULT;
    }
    s[len] = '\0';
    *budget -= size;
    *out = s;
    return 1;
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
    out->envc = 0;
    out->envp = NULL;

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

        long r = copy_user_slot(caller, user_argv_ptr, i, &budget, &out->argv[i]);
        if (r <= 0) {
            exec_args_free(out);
            return r < 0 ? r : -EFAULT;
        }
    }

    return 0;
}

long exec_env_copy_from_user(task_t *caller, uint64_t user_envp_ptr, exec_args_t *out) {
    if (!caller || !out) return -EFAULT;

    out->envc = 0;
    out->envp = kmalloc(sizeof(char *) * EXEC_MAX_ENV);
    if (!out->envp) return -ENOMEM;

    if (!user_envp_ptr)
        return 0;
    if (!user_ptr_valid(user_envp_ptr))
        return -EFAULT;

    size_t budget = EXEC_MAX_ARGV_BYTES;
    for (int i = 0; ; i++) {
        char *s = NULL;
        long r = copy_user_slot(caller, user_envp_ptr, i, &budget, &s);
        if (r <= 0)
            return r;
        if (i >= EXEC_MAX_ENV) {
            kfree(s);
            return -E2BIG;
        }
        out->envp[out->envc++] = s;
    }
}
