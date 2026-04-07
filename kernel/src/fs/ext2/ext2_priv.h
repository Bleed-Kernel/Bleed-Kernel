#pragma once

#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

static int   ext2_lookup  (INode_t *dir,  const char *name, size_t namelen, INode_t **result);
static long  ext2_read    (INode_t *inode, void *buf, size_t count, size_t offset);
static long  ext2_write   (INode_t *inode, const void *buf, size_t count, size_t offset);
static int   ext2_readdir (INode_t *dir,  size_t index, INode_t **result);
static int   ext2_create  (INode_t *parent, const char *name, size_t namelen, INode_t **result, inode_type node_type);
static int   ext2_unlink  (INode_t *dir,  const char *name, size_t namelen);
static int   ext2_rename  (INode_t *dir,  const char *oldname, size_t oldlen, const char *newname, size_t newlen);
static int   ext2_truncate(INode_t *inode, size_t new_size);
static size_t ext2_size   (INode_t *inode);
static void  ext2_drop    (INode_t *inode);