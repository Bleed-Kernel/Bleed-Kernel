#pragma once
#include <fs/vfs.h>
#include <mm/paging.h>
#include <sched/scheduler.h>

#define EXEC_MAX_ARGS      32
#define EXEC_MAX_ARG_LEN   256

int elf_load(INode_t *elf_file, paddr_t cr3, uintptr_t* entry);
int elf_setup_user_args(task_t *task, int argc, const char *const argv[]);

INode_t *elf_get_from_path(const char *path);
task_t *elf_sched(INode_t *file, int argc, const char *const argv[]);
