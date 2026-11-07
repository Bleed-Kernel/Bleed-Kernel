#pragma once
#include <fs/vfs.h>
#include <mm/paging.h>
#include <sched/scheduler.h>

#define EXEC_MAX_ARGS        256
#define EXEC_MAX_PATH_LEN    256
#define EXEC_MAX_ARGV_BYTES  (USER_STACK_SIZE / 4) // linux also uses quater of the stack size
#define EXEC_MAX_ENV         256

typedef struct {
    int argc;
    char **argv;
    int envc;
    char **envp;
} exec_args_t;

long exec_args_copy_from_user(task_t *caller, uint64_t user_argv_ptr, uint64_t user_argc,
                              const char *path, exec_args_t *out);
long exec_env_copy_from_user(task_t *caller, uint64_t user_envp_ptr, exec_args_t *out);
void exec_args_free(exec_args_t *args);

#define ELF_DYN_BASE        0x0000001000000000ULL
#define ELF_INTERP_BASE     0x0000002000000000ULL

// what got loaded, the auxv is built from it
typedef struct {
    uintptr_t entry;
    uintptr_t prog_entry;
    uintptr_t phdr;
    uint16_t  phnum;
    uintptr_t interp_base;
} elf_image_t;

int elf_load(INode_t *elf_file, paddr_t cr3, elf_image_t *image);
int elf_setup_user_args(task_t *task, int argc, const char *const argv[],
                        int envc, const char *const envp[], const elf_image_t *image);

INode_t *elf_get_from_path(const char *path);
task_t *elf_sched(INode_t *file, int argc, const char *const argv[]);
// the same but left blocked so the caller can finish it off, sched_wake_task starts it
task_t *elf_sched_held(INode_t *file, int argc, const char *const argv[]);
