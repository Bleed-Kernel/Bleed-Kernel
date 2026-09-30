#pragma once
#include <fs/vfs.h>
#include <mm/paging.h>
#include <sched/scheduler.h>

#define EXEC_MAX_ARGS        256
#define EXEC_MAX_PATH_LEN    256
#define EXEC_MAX_ARGV_BYTES  (USER_STACK_SIZE / 4) // linux also uses quater of the stack size

typedef struct {
    int argc;
    char **argv;
} exec_args_t;

long exec_args_copy_from_user(task_t *caller, uint64_t user_argv_ptr, uint64_t user_argc,
                              const char *path, exec_args_t *out);
void exec_args_free(exec_args_t *args);

int elf_load(INode_t *elf_file, paddr_t cr3, uintptr_t* entry);
int elf_setup_user_args(task_t *task, int argc, const char *const argv[]);

INode_t *elf_get_from_path(const char *path);
task_t *elf_sched(INode_t *file, int argc, const char *const argv[]);
