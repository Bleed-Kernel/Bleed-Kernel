#include <fs/vfs.h>
#include <string.h>
#include <ansii.h>
#include <stdio.h>
#include <string.h>
#include <status.h>
#include <stdbool.h>
#include <mm/kalloc.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <user/user_file.h>

extern const filesystem tempfs;

fd_table_t *current_fd_table = NULL;

INode_t* vfs_root = NULL;

INode_t* vfs_get_root(){
    if (vfs_root) return vfs_root;
    else return NULL;
}

int vfs_mount_root(){
    int r = tempfs.mount(&vfs_root);
    if (r < 0) {
        serial_printf("%s vfs_mount_root: tempfs.mount failed: %d\n", LOG_ERROR, r);
    }
    serial_printf("%sVFS Root Mounted\n", LOG_OK);

    if (!current_fd_table) {
        current_fd_table = kmalloc(sizeof(fd_table_t));
        if (current_fd_table)
            memset(current_fd_table, 0, sizeof(fd_table_t));
    }

    return r;
}

void vfs_drop(INode_t* inode){
    if (!inode) return;
    if (inode == vfs_get_root()) return;
    if (inode->shared == 0) return;
    inode->shared--;
}

int vfs_lookup(const path_t* path, INode_t** out_inode){
    INode_t* current_inode = path->start;
    const char* head = path->data, *head_end = path->data + path->data_length;
    while(head < head_end) {
        while (head < head_end && *head == '/') head++;
        if (head >= head_end) break;

        const char* comp_start = head;
        while (head < head_end && *head != '/') head++;
        size_t comp_len = head - comp_start;

        INode_t* next = NULL;
        long r = inode_lookup(current_inode, comp_start, comp_len, &next);
        if (r < 0){
            return -FILE_NOT_FOUND;
        }

        current_inode = next;
    }
    if (current_inode){
        current_inode->shared++;
    }else{
        serial_printf(LOG_WARN "Failed to find file %s\n", path->data);
    }
    *out_inode = current_inode;
    return 0;
}

int vfs_create(const path_t* path, INode_t** out_result, inode_type node_type){
    INode_t* parent_inode = NULL;
    path_t parent = vfs_parent_path(path);

    int e = vfs_lookup(&parent, &parent_inode);
    if (e < 0) return e;

    const char* name = parent.data + parent.data_length;
    size_t namelen = path->data_length - parent.data_length;

    e = inode_create(parent_inode, name, namelen, out_result, node_type);
    if (e == 0 && *out_result) {
        (*out_result)->parent = parent_inode;
    }

    vfs_drop(parent_inode);
    return e;
}

path_t vfs_parent_path(const path_t* path){
    const char* end = path->data + path->data_length;
    while(end > path->data && *(end-1) == '/') end--;
    while(end > path->data && *(end-1) != '/') end--;
    return(path_t){
        .root = path->root,
        .start = path->start,
        .data = path->data,
        .data_length = end - path->data,
    };
}

path_t vfs_path_from_abs(const char* path){
    return (path_t){
        .root = vfs_root,
        .start = vfs_root,
        .data = path,
        .data_length = strlen(path),
    };
}

size_t vfs_filesize(INode_t* inode) {
    if (!inode || !inode->ops->read) return 0;

    size_t total = 0;
    size_t offset = 0;
    char buffer[4096];
    long r;

    while ((r = inode_read(inode, buffer, sizeof(buffer), offset)) > 0) {
        total += r;
        offset += r;
    }

    return total;
}

path_t vfs_path_from_relative(const char *path, INode_t *cwd) {
    INode_t *start = cwd ? cwd : vfs_get_root();
    if (!start) start = vfs_get_root();
    if (path[0] == '/') start = vfs_get_root();
    return (path_t){
        .root = vfs_get_root(),
        .start = start,
        .data = path,
        .data_length = strlen(path),
    };
}

long vfs_read_exact(INode_t *inode, void *out_buffer, size_t exact_count, size_t offset){
    while (exact_count > 0){
        long r = inode_read(inode, out_buffer, exact_count, offset);
        if (r < 0) return r;
        if (r == 0) return status_print_error(SHORTREAD);

        out_buffer = (char *)out_buffer + r;
        exact_count -= r;
        offset += r;
    }

    return 0;
}

int vfs_chdir(const char *path_str) {
    if (!path_str) return -FILE_NOT_FOUND;

    task_t *task = get_current_task();
    if (!task) return -1;

    INode_t *start_inode = NULL;

    if (path_str[0] == '/') {
        start_inode = vfs_get_root();
    } else {
        start_inode = task->current_directory ? task->current_directory : vfs_get_root();
    }

    path_t path = (path_t){
        .root = vfs_get_root(),
        .start = start_inode,
        .data = path_str,
        .data_length = strlen(path_str),
    };

    INode_t *inode = NULL;
    int r = vfs_lookup(&path, &inode);
    if (r < 0) return r;

    if (inode->type != INODE_DIRECTORY) {
        vfs_drop(inode);
        return -FILE_NOT_FOUND;
    }

    // Drop old cwd
    if (task->current_directory)
        vfs_drop(task->current_directory);

    task->current_directory = inode;
    inode->shared++; // Increment since task now owns a reference

    return 0;
}

int vfs_open(const char *path_str, int flags){
    if (!current_fd_table) return status_print_error(OUT_OF_BOUNDS);

    path_t path = vfs_path_from_abs(path_str);
    INode_t *inode = NULL;

    int l = vfs_lookup(&path, &inode);
    if (l < 0){
        if (!(flags & O_CREAT)) return l;
        l = vfs_create(&path, &inode, INODE_FILE);
        if (l < 0) return l;
    }

    file_t *f = kmalloc(sizeof(*f));
    if (!f){
        vfs_drop(inode);
        return status_print_error(OUT_OF_MEMORY);
    }

    f->inode = inode;
    f->offset = flags & O_TRUNC ? 0 : ((flags & O_APPEND) ? vfs_filesize(inode) : 0);
    f->flags = flags & (O_MODE | O_APPEND | O_TRUNC);
    f->shared = 1;

    for (int fd = 0; fd < MAX_FDS; fd++){
        if (!current_fd_table->fds[fd]){
            current_fd_table->fds[fd] = f;
            return fd;
        }
    }

    kfree(f, sizeof(*f));
    vfs_drop(inode);
    return status_print_error(OUT_OF_BOUNDS);
}

long vfs_read(int fd, void *buf, size_t count) {
    if (!current_fd_table || fd >= MAX_FDS)
        return -1;

    file_t *f = current_fd_table->fds[fd];
    if (!f)
        return -1;

    int mode = f->flags & O_MODE;
    if (mode != O_RDONLY && mode != O_RDWR)
        return -1;

    uint64_t filesize = vfs_filesize(f->inode);
    if (f->offset >= filesize)
        return 0; // EOF

    if (count > filesize - f->offset)
        count = filesize - f->offset;

    long r = inode_read(f->inode, buf, count, f->offset);
    if (r > 0)
        f->offset += r;

    return r;
}

long vfs_write(int fd, const void *buf, size_t count){
    file_t *f = current_fd_table->fds[fd];
    if (!f) return -1;

    int mode = f->flags & O_MODE;
    if (mode != O_WRONLY && mode != O_RDWR)
        return -1;

    long r = inode_write(f->inode, buf, count, f->offset);
    if (r > 0) f->offset += r;
    return r;
}

int vfs_close(int fd){
    if (!current_fd_table || fd >= MAX_FDS) return status_print_error(FILE_NOT_FOUND);

    file_t *f = current_fd_table->fds[fd];
    if (!f) return status_print_error(FILE_NOT_FOUND);

    current_fd_table->fds[fd] = NULL;

    if (f->shared == 0){
        vfs_drop(f->inode);
        kfree(f, sizeof(*f));
    }

    return 0;
}

int inode_create(INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type){
    if (parent->ops->create == NULL) return status_print_error(UNIMPLEMENTED);
    return parent->ops->create(parent, name, namelen, result, node_type);
}

int inode_lookup(INode_t* dir, const char* name, size_t name_len, INode_t** result){
    if (dir->ops->lookup == NULL) return status_print_error(UNIMPLEMENTED);
    return dir->ops->lookup(dir, name, name_len, result);
}

void inode_drop(INode_t* inode){
    if (inode->ops->drop == NULL) return;
    inode->ops->drop(inode);
}

long inode_write(INode_t* inode, const void* in_buffer, size_t count, size_t offset){
    if (inode->ops->write == NULL) return status_print_error(UNIMPLEMENTED);
    return inode->ops->write(inode, in_buffer, count, offset);
}

long inode_read(INode_t* inode, void* out_buffer, size_t count, size_t offset){
    if (inode->ops->read == NULL) return status_print_error(UNIMPLEMENTED);
    return inode->ops->read(inode, out_buffer, count, offset);
}

int vfs_readdir(INode_t* dir, size_t index, INode_t** result){
    if(!dir || !dir->ops->readdir) return status_print_error(UNIMPLEMENTED);
    return dir->ops->readdir(dir, index, result);
}
