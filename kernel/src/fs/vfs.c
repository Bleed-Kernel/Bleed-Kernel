#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <string.h>
#include <ansii.h>
#include <stdio.h>
#include <string.h>
#include <status.h>
#include <stdbool.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <mm/smap.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <user/user_file.h>
#include <user/errno.h>
#include <mm/spinlock.h>
#include <devices/devices.h>
#include <fs/pipe.h>

extern const filesystem tempfs;

static fd_table_t *boot_fd_table = NULL;

INode_t* vfs_root = NULL;

static fd_table_t *vfs_fd_table_alloc(void) {
    fd_table_t *table = kmalloc(sizeof(fd_table_t));
    if (!table)
        return NULL;
    memset(table, 0, sizeof(fd_table_t));
    
    return table;
}

static fd_table_t *vfs_get_active_fd_table(void) {
    task_t *task = get_current_task();
    if (task && task->fd_table)
        return task->fd_table;

    return boot_fd_table;
}

fd_table_t *vfs_get_kernel_table(void) {
    if (!boot_fd_table)
        boot_fd_table = vfs_fd_table_alloc();
    return boot_fd_table;
}

fd_table_t *vfs_fd_table_clone(const fd_table_t *src) {
    if (!src)
        return vfs_fd_table_alloc();

    fd_table_t *dst = vfs_fd_table_alloc();
    if (!dst)
        return NULL;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_t *f = src->fds[fd];
        dst->fds[fd] = f;
        if (f)
            f->shared++;
    }

    return dst;
}

void vfs_fd_table_drop(fd_table_t *table) {
    if (!table)
        return;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_t *f = table->fds[fd];
        if (!f)
            continue;

        table->fds[fd] = NULL;

        f->shared--;
        if (f->shared <= 0) {
            if (f->type == FD_TYPE_PIPE)
                pipe_file_release(f);
            else if (f->inode) {
                vfs_drop(f->inode);
            }
            kfree(f, sizeof(*f));
        }
    }

    if (table == boot_fd_table)
        boot_fd_table = NULL;

    kfree(table, sizeof(*table));
}

INode_t* vfs_get_root(){
    if (vfs_root) return vfs_root;
    else return NULL;
}

int vfs_mount_root(){
    int r = tempfs.mount(&vfs_root);
    if (r < 0) {
        serial_printf("%s vfs_mount_root: tempfs.mount failed: %d\n", LOG_ERROR, r);
        return r;
    }
    serial_printf("%sVFS Root Mounted\n", LOG_OK);

    path_t devpath = vfs_path_from_abs("/dev");
    INode_t *devinode = NULL;
    int dr = vfs_create(&devpath, &devinode, INODE_DIRECTORY);
    if (dr < 0)
        serial_printf("%s vfs_mount_root: failed to create /dev: %d\n", LOG_ERROR, dr);
    else if (devinode)
        vfs_drop(devinode);

    path_t mntpath = vfs_path_from_abs("/mnt");
    INode_t *mntinode = NULL;
    int mr = vfs_create(&mntpath, &mntinode, INODE_DIRECTORY);
    if (mr < 0)
        serial_printf("%s vfs_mount_root: failed to create /mnt: %d\n", LOG_ERROR, mr);
    else if (mntinode)
        vfs_drop(mntinode);

    (void)vfs_get_kernel_table();

    return 0;
}

void vfs_drop(INode_t* inode){
    if (!inode) return;
    if (inode == vfs_root) return;
    if (inode->shared <= 0) return;
    inode->shared--;
    if (inode->shared == 0) {
        inode_drop(inode);
        kfree(inode, sizeof(*inode));
    }
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

        if (comp_len == 1 && comp_start[0] == '.') {
            continue;
        }
        if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            if (current_inode && current_inode->parent) {
                current_inode = current_inode->parent;
            }
            continue;
        }

        if (!current_inode || !current_inode->ops || !current_inode->ops->lookup) {
            serial_printf(LOG_WARN "vfs_lookup: inode has no lookup op at component '%.*s'\n",
                          (int)comp_len, comp_start);
            return -FILE_NOT_FOUND;
        }

        INode_t* next = NULL;
        long r = inode_lookup(current_inode, comp_start, comp_len, &next);
        if (r < 0){
            return -FILE_NOT_FOUND;
        }
        
        // apply the component name this was my fix for the weird fs issues
        if (next && next->name[0] == '\0') {
            size_t copy_len = comp_len < sizeof(next->name) - 1
                              ? comp_len : sizeof(next->name) - 1;
            memcpy(next->name, comp_start, copy_len);
            next->name[copy_len] = '\0';
        }

        // If "next" is a mount point transparently redirect into the mounted 
        // filesystem's root instead
        INode_t *mounted = vfs_mount_resolve(next);
        if (mounted) {
            if (mounted->name[0] == '\0') {
                size_t copy_len = comp_len < sizeof(mounted->name) - 1
                                  ? comp_len : sizeof(mounted->name) - 1;
                memcpy(mounted->name, comp_start, copy_len);
                mounted->name[copy_len] = '\0';
            }
            next = mounted;
        }

        current_inode = next;
    }
    if (current_inode){ // each caller still gets its own ref
        if (current_inode != vfs_root)
            current_inode->shared++;
    } else {
        serial_printf(LOG_WARN "Failed to find file %s\n", path->data);
        return -FILE_NOT_FOUND;
    }
    *out_inode = current_inode;
    return 0;
}

int vfs_create(const path_t* path, INode_t** out_result, inode_type node_type){
    if (!path || !path->data || path->data_length == 0 || !out_result)
        return status_print_error(FILE_NOT_FOUND);

    const char* path_begin = path->data;
    const char* path_end = path->data + path->data_length;

    while (path_end > path_begin && *(path_end - 1) == '/')
        path_end--;
    if (path_end == path_begin)
        return status_print_error(FILE_NOT_FOUND);

    const char* name = path_end;
    while (name > path_begin && *(name - 1) != '/')
        name--;

    size_t namelen = (size_t)(path_end - name);
    if (namelen == 0)
        return status_print_error(FILE_NOT_FOUND);

    path_t parent = (path_t){
        .root = path->root,
        .start = path->start,
        .data = path->data,
        .data_length = (size_t)(name - path_begin),
    };

    INode_t* parent_inode = NULL;
    int e = vfs_lookup(&parent, &parent_inode);
    if (e < 0) return e;

    e = inode_create(parent_inode, name, namelen, out_result, node_type);
    if (e == 0 && *out_result) {
        (*out_result)->parent = parent_inode;
        (*out_result)->shared++;    // bump ref because it starts with 1, the tree ref but this should also have one

        // Store the component name so getcwd can walk up the tree
        size_t copy_len = namelen < sizeof((*out_result)->name) - 1
                          ? namelen : sizeof((*out_result)->name) - 1;
        memcpy((*out_result)->name, name, copy_len);
        (*out_result)->name[copy_len] = '\0';
    }

    vfs_drop(parent_inode);
    return e;
}

int vfs_ioctl(int fd, unsigned long request, void* arg) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    
    file_t *file = fd_table->fds[fd];
    if (!file || !file->inode) return -1;

    INode_t *inode = file->inode;

    if (inode->ops && inode->ops->ioctl) {
        return inode->ops->ioctl(inode, request, arg);
    }

    return -1;
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
    if (!inode || !inode->ops)
        return 0;
        
    if (inode->ops->size)
        return inode->ops->size(inode); // ideally we should always hit this

    // this fallback is super duper slow

    if (!inode->ops->read)
        return 0;

    size_t total = 0;
    size_t offset = 0;
    char buffer[4096];
    long r;

    while ((r = inode_read(inode, buffer, sizeof(buffer), offset)) > 0) {
        total += (size_t)r;
        offset += (size_t)r;
    }

    return total;
}

static int vfs_parent_and_name(const char *path_str, INode_t *cwd, path_t *out_parent, const char **out_name, size_t *out_namelen) {
    if (!path_str || !*path_str || !out_parent || !out_name || !out_namelen)
        return status_print_error(FILE_NOT_FOUND);

    path_t path = vfs_path_from_relative(path_str, cwd);

    const char *path_begin = path.data;
    const char *path_end = path_begin + path.data_length;

    while (path_end > path_begin && *(path_end - 1) == '/')
        path_end--;
    if (path_end == path_begin)
        return status_print_error(FILE_NOT_FOUND);

    const char *name = path_end;
    while (name > path_begin && *(name - 1) != '/')
        name--;

    size_t namelen = (size_t)(path_end - name);
    if (namelen == 0)
        return status_print_error(FILE_NOT_FOUND);

    *out_parent = (path_t){
        .root = path.root,
        .start = path.start,
        .data = path_begin,
        .data_length = (size_t)(name - path_begin),
    };
    *out_name = name;
    *out_namelen = namelen;
    return 0;
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

    // Drop old cwd. Root is pinned so vfs_drop guards it 
    if (task->current_directory)
        vfs_drop(task->current_directory);
    task->current_directory = inode;

    return 0;
}

int vfs_open(const char *path_str, int flags){
    if (!path_str || path_str[0] == '\0')
        return status_print_error(FILE_NOT_FOUND);
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table) return status_print_error(OUT_OF_BOUNDS);

    if (strncmp(path_str, "/dev/", 5) == 0) {
        const char *dev_name = path_str + 5;
        if (*dev_name != '\0') {
            INode_t *dev_inode = device_get_by_name(dev_name);
            if (dev_inode) {
                file_t *f = kmalloc(sizeof(*f));
                if (!f)
                    return status_print_error(OUT_OF_MEMORY);

                f->type = FD_TYPE_DEV;
                f->inode = dev_inode;
                f->inode->shared++;
                f->offset = 0;
                f->flags = flags & (O_MODE | O_APPEND | O_TRUNC);
                f->shared = 1;

                for (int fd = 0; fd < MAX_FDS; fd++) {
                    if (!fd_table->fds[fd]) {
                        fd_table->fds[fd] = f;
                        return fd;
                    }
                }

                kfree(f, sizeof(*f));
                return status_print_error(OUT_OF_BOUNDS);
            }
        }
    }

    task_t *task = get_current_task();
    INode_t *cwd = task ? task->current_directory : NULL;
    path_t path = vfs_path_from_relative(path_str, cwd);
    INode_t *inode = NULL;

    int l = vfs_lookup(&path, &inode);
    if (l < 0){
        if (!(flags & O_CREAT)) return l;
        l = vfs_create(&path, &inode, INODE_FILE);
        if (l < 0) return l;
        // do not bump here! the file structure owns the caller ref and vfs_create ends up with shared = 2
    }

    file_t *f = kmalloc(sizeof(*f));
    if (!f){
        vfs_drop(inode);
        return status_print_error(OUT_OF_MEMORY);
    }

    if ((flags & O_TRUNC) && inode->type == INODE_FILE) {
        int tr = inode_truncate(inode, 0);
        if (tr < 0) {
            kfree(f, sizeof(*f));
            vfs_drop(inode);
            return tr;
        }
    }

    f->inode = inode;
    f->type = FD_TYPE_FS;
    f->offset = (flags & O_APPEND) ? vfs_filesize(inode) : 0;
    f->flags = flags & (O_MODE | O_APPEND | O_TRUNC);
    f->shared = 1;

    for (int fd = 0; fd < MAX_FDS; fd++){
        if (!fd_table->fds[fd]){
            fd_table->fds[fd] = f;
            return fd;
        }
    }

    kfree(f, sizeof(*f));
    vfs_drop(inode);
    return status_print_error(OUT_OF_BOUNDS);
}

long vfs_read(int fd, void *buf, size_t count) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return -1;

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops)
        return -2;

    // guard agianst dangling nodes
    if (f->type == FD_TYPE_FS && !f->inode->internal_data)
        return -2;

    uintptr_t inode_addr = (uintptr_t)f->inode;
    uintptr_t ops_addr   = (uintptr_t)f->inode->ops;
    if (inode_addr < 0xFFFF800000000000ULL || ops_addr < 0xFFFF800000000000ULL)
        return -2;

    int mode = f->flags & O_MODE;
    if (mode != O_RDONLY && mode != O_RDWR)
        return -3;

    bool is_size_finite = false;
    uint64_t filesize = 0;

    if (f->inode->ops && f->inode->ops->size) {
        is_size_finite = true;
        filesize = f->inode->ops->size(f->inode);
    } else if (f->type == FD_TYPE_FS && f->inode->type == INODE_FILE) {
        is_size_finite = true;
        filesize = vfs_filesize(f->inode);
    }

    if (is_size_finite) {
        if (f->offset >= filesize)
            return 0;

        if (count > filesize - f->offset)
            count = filesize - f->offset;
    }

    for (;;) {
        task_t *current = get_current_task();
        if (signal_should_interrupt(current))
            return -EINTR;

        long r = inode_read(f->inode, buf, count, f->offset);

        if (r > 0) {
            if (is_size_finite)
                f->offset += r;
            return r;
        }

        if (r < 0)
            return r;

        if (f->type == FD_TYPE_PIPE)
            return 0;

        if (is_size_finite)
            return 0;

        // Block the task rather than spinning while we wait for device data
        if (current) {
            sched_block(current);
        } else {
            sched_yield(current);
        }

        if (signal_should_interrupt(current))
            return -EINTR;
    }
}

long vfs_write(int fd, const void *buf, size_t count){
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return -1;

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops)
        return -1;

    uintptr_t inode_addr = (uintptr_t)f->inode;
    uintptr_t ops_addr   = (uintptr_t)f->inode->ops;
    if (inode_addr < 0xFFFF800000000000ULL || ops_addr < 0xFFFF800000000000ULL)
        return -1;

    int mode = f->flags & O_MODE;
    if (mode != O_WRONLY && mode != O_RDWR)
        return -1;

    long r = inode_write(f->inode, buf, count, f->offset);
    if (r > 0 && f->type != FD_TYPE_PIPE)
        f->offset += r;
    return r;
}

int vfs_unlink(const char *path_str) {
    task_t *task = get_current_task();
    INode_t *cwd = task ? task->current_directory : NULL;

    path_t parent_path;
    const char *name = NULL;
    size_t namelen = 0;

    int r = vfs_parent_and_name(path_str, cwd, &parent_path, &name, &namelen);
    if (r < 0)
        return r;

    INode_t *parent_inode = NULL;
    r = vfs_lookup(&parent_path, &parent_inode);
    if (r < 0)
        return r;

    if (!parent_inode->ops || !parent_inode->ops->unlink) {
        vfs_drop(parent_inode);
        return status_print_error(UNIMPLEMENTED);
    }

    r = parent_inode->ops->unlink(parent_inode, name, namelen);
    vfs_drop(parent_inode);
    return r;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    task_t *task = get_current_task();
    INode_t *cwd = task ? task->current_directory : NULL;

    path_t old_parent_path;
    const char *oldname = NULL;
    size_t oldlen = 0;
    int r = vfs_parent_and_name(oldpath, cwd, &old_parent_path, &oldname, &oldlen);
    if (r < 0)
        return r;

    path_t new_parent_path;
    const char *newname = NULL;
    size_t newlen = 0;
    r = vfs_parent_and_name(newpath, cwd, &new_parent_path, &newname, &newlen);
    if (r < 0)
        return r;

    INode_t *old_parent = NULL;
    r = vfs_lookup(&old_parent_path, &old_parent);
    if (r < 0)
        return r;

    INode_t *new_parent = NULL;
    r = vfs_lookup(&new_parent_path, &new_parent);
    if (r < 0) {
        vfs_drop(old_parent);
        return r;
    }

    if (old_parent != new_parent) {
        vfs_drop(old_parent);
        vfs_drop(new_parent);
        return -EXDEV;
    }

    if (!old_parent->ops || !old_parent->ops->rename) {
        vfs_drop(old_parent);
        vfs_drop(new_parent);
        return status_print_error(UNIMPLEMENTED);
    }

    r = old_parent->ops->rename(old_parent, oldname, oldlen, newname, newlen);

    vfs_drop(old_parent);
    vfs_drop(new_parent);
    return r;
}

int vfs_mkdir(const char *path_str) {
    task_t *task = get_current_task();
    INode_t *cwd = task ? task->current_directory : NULL;
    path_t path = vfs_path_from_relative(path_str, cwd);

    INode_t *existing = NULL;
    if (vfs_lookup(&path, &existing) == 0) {
        vfs_drop(existing);
        return -EEXIST;
    }

    INode_t *inode = NULL;
    int r = vfs_create(&path, &inode, INODE_DIRECTORY);
    if (r < 0)
        return r;

    if (inode)
        vfs_drop(inode); // release caller ref tree holds its own

    return 0;
}

int vfs_pipe(int out_fds[2]) {
    if (!out_fds)
        return -1;

    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table)
        return -1;

    int read_fd = -1;
    int write_fd = -1;

    // Preserve stdio slots (0,1,2) for stdin stdout stderr
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table->fds[i]) {
            if (read_fd < 0)
                read_fd = i;
            else {
                write_fd = i;
                break;
            }
        }
    }

    if (read_fd < 0 || write_fd < 0)
        return -1;

    file_t *read_file = NULL;
    file_t *write_file = NULL;
    if (pipe_create_file_pair(&read_file, &write_file) != 0)
        return -1;

    fd_table->fds[read_fd] = read_file;
    fd_table->fds[write_fd] = write_file;
    out_fds[0] = read_fd;
    out_fds[1] = write_fd;
    return 0;
}

int vfs_dup2(int oldfd, int newfd) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return -1;

    file_t *src = fd_table->fds[oldfd];
    if (!src)
        return -1;

    if (oldfd == newfd)
        return newfd;

    if (fd_table->fds[newfd]) {
        int rc = vfs_close(newfd);
        if (rc < 0)
            return rc;
    }

    src->shared++;
    fd_table->fds[newfd] = src;
    return newfd;
}

int vfs_close(int fd){
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS) return status_print_error(FILE_NOT_FOUND);

    file_t *f = fd_table->fds[fd];
    if (!f) return status_print_error(FILE_NOT_FOUND);

    fd_table->fds[fd] = NULL;

    f->shared--;
    if (f->shared <= 0){
        if (f->type == FD_TYPE_PIPE) {
            pipe_file_release(f);
        } else {
            vfs_drop(f->inode);
        }
        kfree(f, sizeof(*f));
    }

    return 0;
}

long vfs_seek(int fd, long offset, int whence) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return status_print_error(FILE_NOT_FOUND);

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode)
        return status_print_error(FILE_NOT_FOUND);

    long new_offset;

    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;

        case SEEK_CUR:
            new_offset = (long)f->offset + offset;
            break;

        case SEEK_END:
            new_offset = (long)vfs_filesize(f->inode) + offset;
            break;

        default:
            return -2;
    }

    if (new_offset < 0)
        return -2;

    f->offset = (size_t)new_offset;
    return new_offset;
}

user_file_t *vfs_file_stat(int fd) {
    // userfacing structure and function kernel wont really need this
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return NULL;

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode)
        return NULL;

    user_file_t *stat = kmalloc(sizeof(*stat));
    if (!stat)
        return NULL;

    memset(stat, 0, sizeof(*stat));

    stat->filesize    = vfs_filesize(f->inode);
    stat->permissions = f->flags & O_MODE;

    // fname -> internal inode name.
    if (f->inode->internal_data) {
        const char *name = (const char *)f->inode->internal_data;
        strncpy(stat->fname, name, sizeof(stat->fname) - 1);
    }

    return stat;
}

int inode_create(INode_t* parent, const char* name, size_t namelen, INode_t** result, inode_type node_type){
    if (!parent || !parent->ops || !parent->ops->create) return status_print_error(UNIMPLEMENTED);
    return parent->ops->create(parent, name, namelen, result, node_type);
}

int inode_lookup(INode_t* dir, const char* name, size_t name_len, INode_t** result){
    if (!dir || !dir->ops || !dir->ops->lookup) return status_print_error(UNIMPLEMENTED);
    return dir->ops->lookup(dir, name, name_len, result);
}

void inode_drop(INode_t* inode){
    if (!inode || !inode->ops || !inode->ops->drop) return;
    inode->ops->drop(inode);
}

int inode_truncate(INode_t* inode, size_t new_size){
    if (!inode || !inode->ops || !inode->ops->truncate) return status_print_error(UNIMPLEMENTED);
    return inode->ops->truncate(inode, new_size);
}

long inode_write(INode_t* inode, const void* in_buffer, size_t count, size_t offset){
    if (!inode || !inode->ops || !inode->ops->write) return status_print_error(UNIMPLEMENTED);
    return inode->ops->write(inode, in_buffer, count, offset);
}

long inode_read(INode_t* inode, void* out_buffer, size_t count, size_t offset){
    if (!inode || !inode->ops || !inode->ops->read) return status_print_error(UNIMPLEMENTED);
    return inode->ops->read(inode, out_buffer, count, offset);
}

int vfs_readdir(INode_t* dir, size_t index, INode_t** result){
    if(!dir || !dir->ops || !dir->ops->readdir) return status_print_error(UNIMPLEMENTED);
    return dir->ops->readdir(dir, index, result);
}