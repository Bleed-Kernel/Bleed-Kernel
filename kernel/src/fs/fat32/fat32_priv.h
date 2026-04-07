#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

static int fat32_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result);
static long fat32_read(INode_t *inode, void *buf, size_t count, size_t offset);
static long fat32_write(INode_t *inode, const void *buf, size_t count, size_t offset);
static int fat32_readdir(INode_t *dir, size_t index, INode_t **result);
static int fat32_create(INode_t *parent, const char *name, size_t namelen, INode_t **result, inode_type node_type);
static int fat32_unlink(INode_t *dir, const char *name, size_t namelen);
static int fat32_rename(INode_t *dir, const char *oldname, size_t oldlen, const char *newname, size_t newlen);
static int fat32_truncate(INode_t *inode, size_t new_size);
static size_t fat32_size(INode_t *inode);
static void fat32_drop(INode_t *inode);