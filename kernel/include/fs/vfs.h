#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <devices/devices.h>
#include <user/user_file.h>

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_MODE   0x3
#define O_CREAT  0x4
#define O_TRUNC  0x8
#define O_APPEND 0x10

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define PATH_MAX    4096
#define MAX_FDS     256

typedef struct fd_table fd_table_t;

typedef struct INode INode_t;

typedef enum {
    INODE_DIRECTORY,
    INODE_FILE,
    INODE_DEVICE
} inode_type;

typedef enum {
    FD_TYPE_FS,
    FD_TYPE_DEV
} fd_type_t;

typedef struct INodeOps{
    int    (*create)  (INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type);
    long   (*read)    (INode_t* inode, void* out_buffer, size_t size, size_t offset);
    long   (*write)   (INode_t* inode, const void* in_buffer, size_t size, size_t offset);
    int    (*lookup)  (INode_t* dir, const char* name, size_t name_length, INode_t** result);
    int    (*ioctl)   (INode_t* inode, unsigned long request, void* arg);
    void   (*drop)    (INode_t* inode);
    int    (*readdir) (INode_t* dir, size_t index, INode_t** result);
    size_t (*size)    (INode_t* inode);
} INodeOps_t;

typedef struct INode {
    long    shared;
    int     type;
    const   INodeOps_t* ops;
    void*   internal_data;
    struct  INode* parent;
} INode_t;

typedef struct file {
    fd_type_t type;
    INode_t *inode;
    
    size_t offset;
    int flags;
    int shared;
} file_t;

typedef struct fd_table{
    file_t *fds[MAX_FDS];
}fd_table_t;

typedef struct filesystem {
    int (*mount)(INode_t** root);
} filesystem;

typedef struct path{
    INode_t    *root, *start;
    const char *data;
    size_t      data_length;
} path_t;

typedef struct dirent {
    char name[256];
    int type;
} dirent_t;

/// @brief mount the root directory of the fs
/// @param root root node
/// @return success?
int tempfs_mount_root(INode_t** root);

/// @brief decrement shared, min 0, cannot drop VFS root
/// @param inode target
INode_t* vfs_get_root();

/// @brief VFS Mount the root directory
/// @return success?
int vfs_mount_root();

/// @brief decrement shared, min 0, cannot drop VFS root
/// @param inode target
void vfs_drop(INode_t* inode);

/// @brief get an inode at a path
/// @param path target path
/// @param inode OUT inode
/// @return success?
int vfs_lookup(const path_t* path, INode_t** inode);

long vfs_read_exact(INode_t *inode, void *out_buffer, size_t exact_count, size_t offset);

int vfs_create(const path_t* path, INode_t** result, inode_type node_type);
int vfs_readdir (INode_t* dir, size_t index, INode_t** result);
size_t vfs_filesize(INode_t* inode);

path_t vfs_path_from_abs(const char* pstring);
path_t vfs_path_from_relative(const char *path, INode_t *cwd); 
path_t vfs_parent_path(const path_t* path);
user_file_t *vfs_file_stat(int fd);

int vfs_open(const char *path_str, int flags);
long vfs_read(int fd, void *buf, size_t count);
long vfs_write(int fd, const void *buf, size_t count);
int vfs_chdir(const char *path_str);
int vfs_close(int fd);
long vfs_seek(int fd, long offset, int whence);
int vfs_ioctl(int fd, unsigned long request, void* arg);

fd_table_t *vfs_get_kernel_table(void);
fd_table_t *vfs_fd_table_clone(const fd_table_t *src);
void vfs_fd_table_drop(fd_table_t *table);

int inode_create(INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type);
long inode_read(INode_t* inode, void* buf, size_t count, size_t offset);
long inode_write(INode_t* inode, const void* buf, size_t count, size_t offset);
int inode_lookup(INode_t* dir, const char* name, size_t name_len, INode_t** ret);
void inode_drop(INode_t* inode);
