#pragma once
#include <stddef.h>
#include <fs/vfs.h>

typedef struct filesystem filesystem;
extern const filesystem tempfs;

// point an empty tempfs file at contents already in memory, nothing is copied until its written to
int tempfs_adopt(INode_t* inode, const void* contents, size_t size);
