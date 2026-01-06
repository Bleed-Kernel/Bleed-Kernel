#pragma once
#include <fs/vfs.h>

int elf_load(INode_t *elf_file, paddr_t cr3, uintptr_t* entry);

INode_t *elf_get_from_path(const char *path);
task_t *elf_sched(INode_t *file);