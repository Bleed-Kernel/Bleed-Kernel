#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

static int    exfat_lookup  (INode_t *dir, const char *name, size_t namelen, INode_t **result);
static long   exfat_read    (INode_t *inode, void *buf, size_t count, size_t offset);
static long   exfat_write   (INode_t *inode, const void *buf, size_t count, size_t offset);
static int    exfat_readdir (INode_t *dir, size_t index, INode_t **result);
static int    exfat_create  (INode_t *parent, const char *name, size_t namelen, INode_t **result, inode_type node_type);
static int    exfat_unlink  (INode_t *dir, const char *name, size_t namelen);
static int    exfat_rename  (INode_t *dir, const char *oldname, size_t oldlen, const char *newname, size_t newlen);
static int    exfat_truncate(INode_t *inode, size_t new_size);
static size_t exfat_size    (INode_t *inode);
static void   exfat_drop    (INode_t *inode);