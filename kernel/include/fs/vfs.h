#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <devices/devices.h>
#include <user/user_stat.h>
#include <sched/wait_queue.h>

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_MODE   0x3
// linux values
#define O_CREAT     0x40
#define O_EXCL      0x80    // with O_CREAT, fail if its already there
#define O_NOCTTY    0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_NONBLOCK  0x800
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000

#define FD_CLOEXEC  1

#define AT_FDCWD (-100)

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
    INODE_DEVICE,
    INODE_SYMLINK
} inode_type;

#define AT_SYMLINK_NOFOLLOW 0x100
#define VFS_LINK_DEPTH      8
#define VFS_LINK_MAX        1024

typedef enum {
    FD_TYPE_FS,
    FD_TYPE_DEV,
    FD_TYPE_PIPE,
    FD_TYPE_SOCKET,
    FD_TYPE_UNIX,
    FD_TYPE_EPOLL
} fd_type_t;

struct file;

typedef struct INodeOps{
    int    (*create)  (INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type);
    long   (*read)    (INode_t* inode, void* out_buffer, size_t size, size_t offset);
    long   (*write)   (INode_t* inode, const void* in_buffer, size_t size, size_t offset);
    int    (*truncate)(INode_t* inode, size_t new_size);
    int    (*lookup)  (INode_t* dir, const char* name, size_t name_length, INode_t** result);
    int    (*unlink)  (INode_t* dir, const char* name, size_t name_length);
    int    (*rename)  (INode_t* dir, const char* oldname, size_t oldlen, const char* newname, size_t newlen);
    int    (*ioctl)   (INode_t* inode, unsigned long request, void* arg);
    void   (*drop)    (INode_t* inode);
    int    (*readdir) (INode_t* dir, size_t index, INode_t** result);
    size_t (*size)    (INode_t* inode);
    int    (*poll)    (INode_t* inode);   // POLL* bits ready right now
    wait_queue_t *(*wait_queue)(INode_t* inode);
    void   (*times)   (INode_t* inode, int64_t *atime, int64_t *mtime, int64_t *ctime);
    int    (*statfs)  (INode_t* root, uint64_t *block_size, uint64_t *total, uint64_t *free);
    int    (*label)   (INode_t* root, char *out, size_t size);    // the volumes name, -ENOENT if it hasnt got one

    // for a device that keeps something per open (the audio card has a stream for each)
    // clsoed when the last ref is used
    int    (*file_ioctl)  (struct file* file, unsigned long request, void* arg);
    void   (*file_release)(struct file* file);

    // a symlink called name in dir pointing at target, and reading one back (length, no nul)
    int    (*symlink) (INode_t* dir, const char* name, size_t namelen, const char* target, INode_t** result);
    long   (*readlink)(INode_t* inode, char* out, size_t size);
} INodeOps_t;

/*
 * An INode doubles as its own dentry, theres no hard links so the two would always be 1:1.
 * Ownership runs up the tree: a child holds a ref on its parent for as long as it lives, so
 * holding any inode keeps every directory above it alive. lookup, create and readdir all hand
 * the caller one ref, vfs_drop gives it back and the last one out frees the inode and lets go
 * of the parent in turn.
 */
typedef struct INode {
    long    shared;
    int     type;
    const   INodeOps_t* ops;
    void*   internal_data;
    struct  INode* parent;      // the vfs holds a ref on this once attached is set
    char    name[256];          // name under parent, set by the vfs when it attaches us
    struct  INode* mounted;     // root of the filesystem mounted on this directory, NULL if none
    struct  INode* dcache_next; // next in the dentry cache bucket
    uint8_t attached;           // we hold a ref on parent
    uint8_t hashed;             // findable in the dentry cache
    uint16_t dcache_bucket;     // the bucket we went in, a filesystem can rename us in place
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
    uint8_t fd_flags[MAX_FDS];
    int refs;   // threads share one
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

/// @brief get the root inode of the VFS
/// @return root inode, NULL before vfs_mount_root
INode_t* vfs_get_root(void);

/// @brief the absolute path of inode, 0 on success or -1 if it doesnt fit or isnt under the root
int vfs_inode_path(INode_t *inode, char *out, size_t size);

/// @brief VFS Mount the root directory
/// @return success?
int vfs_mount_root(void);

/// @brief take a ref on an inode, the VFS root is pinned and never counted
/// @param inode target
void vfs_hold(INode_t* inode);

/// @brief give a ref back, the last one frees the inode and drops its hold on the parent
/// @param inode target
void vfs_drop(INode_t* inode);

/// @brief get an inode at a path
/// @param path target path
/// @param inode OUT inode, the caller owns one ref and has to vfs_drop it
/// @return 0 or a negative errno
int vfs_lookup(const path_t* path, INode_t** inode);

/// @brief vfs_lookup but a symlink at the end of the path is handed back instead of followed
int vfs_lookup_nofollow(const path_t* path, INode_t** inode);

long vfs_read_exact(INode_t *inode, void *out_buffer, size_t exact_count, size_t offset);

int vfs_create(const path_t* path, INode_t** result, inode_type node_type);
int vfs_readdir (INode_t* dir, size_t index, INode_t** result);
size_t vfs_filesize(INode_t* inode);

path_t vfs_path_from_abs(const char* pstring);
path_t vfs_path_from_relative(const char *path, INode_t *cwd); 
path_t vfs_parent_path(const path_t* path);

int vfs_stat(const char *path_str, user_stat_t *st);
int vfs_stat_at(int dirfd, const char *path_str, user_stat_t *st);
int vfs_fstat(int fd, user_stat_t *st);
int vfs_lstat_at(int dirfd, const char *path_str, user_stat_t *st);

int  vfs_symlink_at(const char *target, int dirfd, const char *path_str);
long vfs_readlink_at(int dirfd, const char *path_str, char *out, size_t size);

int vfs_open(const char *path_str, int flags);
int vfs_open_at(int dirfd, const char *path_str, int flags);
long vfs_read(int fd, void *buf, size_t count);
long vfs_write(int fd, const void *buf, size_t count);
long vfs_pread(int fd, void *buf, size_t count, size_t offset);
long vfs_pwrite(int fd, const void *buf, size_t count, size_t offset);
int vfs_unlink(const char *path_str);
int vfs_unlink_at(int dirfd, const char *path_str);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_mkdir(const char *path_str);
int vfs_pipe(int out_fds[2]);

int vfs_install(file_t *f);

// the file behind fd with a ref taken on it, NULL if theres none
file_t *vfs_file_get(int fd);

// give back a ref, the last one closes whatever it is
void vfs_file_put(file_t *f);
int vfs_dup2(int oldfd, int newfd);
int vfs_dup(int oldfd);

// the lowest free fd at or above lowest pointing at the same file
int vfs_dup_from(int oldfd, int lowest);

// FD_CLOEXEC and friends, -EBADF if fd isnt open
int vfs_get_fd_flags(int fd);
int vfs_set_fd_flags(int fd, int flags);
void vfs_close_on_exec(fd_table_t *table);
int vfs_set_nonblock(int fd);
int vfs_chdir(const char *path_str);

// put a task back at the root, letting go of wherever it was
struct task;
void vfs_chdir_root(struct task *task);
int vfs_close(int fd);
long vfs_seek(int fd, long offset, int whence);
int vfs_ioctl(int fd, unsigned long request, void* arg);

fd_table_t *vfs_get_kernel_table(void);
fd_table_t *vfs_fd_table_clone(const fd_table_t *src);
void vfs_fd_table_drop(fd_table_t *table);

fd_table_t *vfs_fd_table_share(fd_table_t *table);

int inode_create(INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type);
long inode_read(INode_t* inode, void* buf, size_t count, size_t offset);
long inode_write(INode_t* inode, const void* buf, size_t count, size_t offset);
int inode_truncate(INode_t* inode, size_t new_size);
int inode_lookup(INode_t* dir, const char* name, size_t name_len, INode_t** ret);
void inode_drop(INode_t* inode);