#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <string.h>
#include <ansii.h>
#include <stdio.h>
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

// let go of one fd's hold on a file, the last one out releases whats behind it
static void vfs_file_put(file_t *f) {
    f->shared--;
    if (f->shared > 0)
        return;

    if (f->type == FD_TYPE_PIPE)
        pipe_file_release(f);
    else
        vfs_drop(f->inode);
    kfree(f);
}

// put a file in the lowest free fd slot
static int vfs_fd_install(fd_table_t *table, file_t *f) {
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (!table->fds[fd]) {
            table->fds[fd] = f;
            return fd;
        }
    }
    return -1;
}

void vfs_fd_table_drop(fd_table_t *table) {
    if (!table)
        return;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_t *f = table->fds[fd];
        if (!f)
            continue;

        table->fds[fd] = NULL;
        vfs_file_put(f);
    }

    if (table == boot_fd_table)
        boot_fd_table = NULL;

    kfree(table);
}

INode_t* vfs_get_root(void){
    return vfs_root;
}

int vfs_mount_root(void){
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

static void vfs_attach(INode_t *child, INode_t *parent, const char *name, size_t namelen);

// dentry cache
//
// (parent, name) -> inode for everything thats currently alive. a second lookup of the same
// name gets the same object back instead of the filesystem building a fresh one, which is what
// lets a mountpoint or an open file be recognised by pointer

#define DCACHE_BUCKETS 256

static INode_t *dcache[DCACHE_BUCKETS];

static size_t dcache_hash(const INode_t *parent, const char *name, size_t len) {
    size_t h = (uintptr_t)parent >> 4;
    for (size_t i = 0; i < len; i++)
        h = h * 31 + (unsigned char)name[i];
    return h % DCACHE_BUCKETS;
}

static INode_t *dcache_find(const INode_t *parent, const char *name, size_t len) {
    for (INode_t *n = dcache[dcache_hash(parent, name, len)]; n; n = n->dcache_next) {
        if (n->parent == parent && strlen(n->name) == len && memcmp(n->name, name, len) == 0)
            return n;
    }
    return NULL;
}

static void dcache_insert(INode_t *inode) {
    if (inode->hashed) return;

    size_t b = dcache_hash(inode->parent, inode->name, strlen(inode->name));
    inode->dcache_next = dcache[b];
    dcache[b] = inode;
    inode->hashed = 1;
}

static void dcache_remove(INode_t *inode) {
    if (!inode->hashed) return;

    INode_t **link = &dcache[dcache_hash(inode->parent, inode->name, strlen(inode->name))];
    while (*link && *link != inode)
        link = &(*link)->dcache_next;
    if (*link)
        *link = inode->dcache_next;

    inode->dcache_next = NULL;
    inode->hashed = 0;
}

// hang a child off its parent: this is where the child takes its ref on the parent
static void vfs_attach(INode_t *child, INode_t *parent, const char *name, size_t namelen) {
    if (child->attached) return;

    if (name != child->name) {
        size_t copy_len = namelen < sizeof(child->name) - 1
                          ? namelen : sizeof(child->name) - 1;
        memcpy(child->name, name, copy_len);
        child->name[copy_len] = '\0';
    }

    child->parent = parent;
    vfs_hold(parent);
    child->attached = 1;

    // a name we had to cut short cant be matched again, leave it out of the cache
    if (namelen < sizeof(child->name))
        dcache_insert(child);
}

// take in an inode a filesystem just handed us. if its one we already know by that name the
// fresh copy is thrown away and the cached one comes back, otherwise it gets attached
static INode_t *vfs_adopt(INode_t *parent, INode_t *child, const char *name, size_t namelen) {
    if (child->attached) return child;

    // go by the filesystems own spelling when it has one, fat ignores case and the cache doesnt
    if (child->name[0] != '\0') {
        name = child->name;
        namelen = strlen(child->name);
    }

    INode_t *cached = dcache_find(parent, name, namelen);
    if (cached && cached != child) {
        vfs_hold(cached);
        vfs_drop(child);
        return cached;
    }

    vfs_attach(child, parent, name, namelen);
    return child;
}

void vfs_hold(INode_t* inode){
    if (!inode || inode == vfs_root) return;
    inode->shared++;
}

void vfs_drop(INode_t* inode){
    // a loop and not recursion, freeing a deep path walks all the way back up the chain
    while (inode && inode != vfs_root) {
        if (inode->shared <= 0) return;
        inode->shared--;
        if (inode->shared > 0) return;

        INode_t *parent = inode->attached ? inode->parent : NULL;

        dcache_remove(inode);
        inode_drop(inode);
        kfree(inode);

        // our hold on the parent goes with us
        inode = parent;
    }
}

int vfs_lookup(const path_t* path, INode_t** out_inode){
    // cur is always a ref we own, each step trades it for a ref on the next inode
    INode_t* cur = path->start;
    if (!cur) return -ENOENT;
    vfs_hold(cur);

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
            // the root of a mount has the mountpoints parent as its parent, so this crosses back out
            if (cur->parent) {
                INode_t *up = cur->parent;
                vfs_hold(up);
                vfs_drop(cur);
                cur = up;
            }
            continue;
        }

        if (!cur->ops || !cur->ops->lookup) {
            vfs_drop(cur);
            return -ENOTDIR;
        }

        INode_t* next = dcache_find(cur, comp_start, comp_len);
        if (next) {
            vfs_hold(next);
        } else {
            long r = inode_lookup(cur, comp_start, comp_len, &next);
            if (r < 0 || !next) {
                vfs_drop(cur);
                return -ENOENT;
            }
            next = vfs_adopt(cur, next, comp_start, comp_len);
        }

        // something is mounted on this directory, carry on from the root of that filesystem
        if (next->mounted) {
            INode_t *root = next->mounted;
            vfs_hold(root);
            vfs_drop(next);
            next = root;
        }

        // next keeps cur alive through its parent ref
        vfs_drop(cur);
        cur = next;
    }

    // whats in the /dev directory is a tempfs placeholder wearing the devices ops, calling
    // those ops on it would hand a driver tempfs data. swap in the real inode from the device list
    if (cur->type == INODE_DEVICE) {
        INode_t *dev = device_get_by_name(cur->name);
        if (dev && dev != cur) {
            vfs_hold(dev);
            vfs_drop(cur);
            cur = dev;
        }
    }

    *out_inode = cur;
    return 0;
}

static int vfs_path_split(const path_t *path, path_t *out_parent, const char **out_name, size_t *out_namelen) {
    const char *path_begin = path->data;
    const char *path_end = path_begin + path->data_length;

    while (path_end > path_begin && *(path_end - 1) == '/')
        path_end--;
    if (path_end == path_begin)
        return -ENOENT;

    const char *name = path_end;
    while (name > path_begin && *(name - 1) != '/')
        name--;

    size_t namelen = (size_t)(path_end - name);
    if (namelen == 0)
        return -ENOENT;

    *out_parent = (path_t){
        .root = path->root,
        .start = path->start,
        .data = path_begin,
        .data_length = (size_t)(name - path_begin),
    };
    *out_name = name;
    *out_namelen = namelen;
    return 0;
}

int vfs_create(const path_t* path, INode_t** out_result, inode_type node_type){
    if (!path || !path->data || path->data_length == 0 || !out_result)
        return -ENOENT;

    path_t parent;
    const char *name = NULL;
    size_t namelen = 0;

    int e = vfs_path_split(path, &parent, &name, &namelen);
    if (e < 0) return e;

    INode_t* parent_inode = NULL;
    e = vfs_lookup(&parent, &parent_inode);
    if (e < 0) return e;

    // the filesystem hands back a ref for the caller, attaching gives it its name and its hold on the parent
    e = inode_create(parent_inode, name, namelen, out_result, node_type);
    if (e == 0 && *out_result)
        vfs_attach(*out_result, parent_inode, name, namelen);

    vfs_drop(parent_inode);
    return e;
}

int vfs_ioctl(int fd, unsigned long request, void* arg) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table) return -EBADF;
    if (fd < 0 || fd >= MAX_FDS) return -EBADF;
    
    file_t *file = fd_table->fds[fd];
    if (!file || !file->inode) return -EBADF;

    INode_t *inode = file->inode;

    if (inode->ops && inode->ops->ioctl) {
        return inode->ops->ioctl(inode, request, arg);
    }

    return -ENOTTY;
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

    // only a real file has an end to find, reading a device until it runs dry never returns
    if (!inode->ops->read || inode->type != INODE_FILE)
        return 0;

    size_t total = 0;
    size_t offset = 0;
    char buffer[512];
    long r;

    while ((r = inode_read(inode, buffer, sizeof(buffer), offset)) > 0) {
        total += (size_t)r;
        offset += (size_t)r;
    }

    return total;
}

static int vfs_parent_and_name(const char *path_str, INode_t *cwd, path_t *out_parent, const char **out_name, size_t *out_namelen) {
    if (!path_str || !*path_str || !out_parent || !out_name || !out_namelen)
        return -ENOENT;

    path_t path = vfs_path_from_relative(path_str, cwd);
    return vfs_path_split(&path, out_parent, out_name, out_namelen);
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
        if (r == 0) return -EIO;

        out_buffer = (char *)out_buffer + r;
        exact_count -= r;
        offset += r;
    }

    return 0;
}

int vfs_chdir(const char *path_str) {
    if (!path_str) return -ENOENT;

    task_t *task = get_current_task();
    if (!task) return -ESRCH;

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
        return -ENOTDIR;
    }

    // Drop old cwd. Root is pinned so vfs_drop guards it 
    if (task->current_directory)
        vfs_drop(task->current_directory);
    task->current_directory = inode;

    return 0;
}

int vfs_open(const char *path_str, int flags){
    if (!path_str || path_str[0] == '\0')
        return -ENOENT;
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table) return -EMFILE;

    task_t *task = get_current_task();
    INode_t *cwd = task ? task->current_directory : NULL;
    path_t path = vfs_path_from_relative(path_str, cwd);
    INode_t *inode = NULL;

    int l = vfs_lookup(&path, &inode);
    if (l < 0){
        if (!(flags & O_CREAT)) return l;
        l = vfs_create(&path, &inode, INODE_FILE);
        if (l < 0) return l;
        // do not bump here! the file structure takes over the callers ref
    }

    // vfs_lookup already swapped any /dev placeholder for the real device inode
    bool is_device = (inode->type == INODE_DEVICE);

    file_t *f = kmalloc(sizeof(*f));
    if (!f){
        vfs_drop(inode);
        return -ENOMEM;
    }

    if ((flags & O_TRUNC) && inode->type == INODE_FILE) {
        int tr = inode_truncate(inode, 0);
        if (tr < 0) {
            kfree(f);
            vfs_drop(inode);
            return tr;
        }
    }

    f->inode = inode;
    f->type = is_device ? FD_TYPE_DEV : FD_TYPE_FS;
    f->offset = (!is_device && (flags & O_APPEND)) ? vfs_filesize(inode) : 0;
    f->flags = flags & (O_MODE | O_APPEND | O_TRUNC);
    f->shared = 1;

    int fd = vfs_fd_install(fd_table, f);
    if (fd >= 0)
        return fd;

    kfree(f);
    vfs_drop(inode);
    return -EMFILE;
}

long vfs_read(int fd, void *buf, size_t count) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return -EBADF;

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops)
        return -EBADF;

    // guard agianst dangling nodes
    if (f->type == FD_TYPE_FS && !f->inode->internal_data)
        return -EBADF;

    uintptr_t inode_addr = (uintptr_t)f->inode;
    uintptr_t ops_addr   = (uintptr_t)f->inode->ops;
    if (inode_addr < 0xFFFF800000000000ULL || ops_addr < 0xFFFF800000000000ULL)
        return -EBADF;

    int mode = f->flags & O_MODE;
    if (mode != O_RDONLY && mode != O_RDWR)
        return -EBADF;

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

        // nothing wakes a BLOCKED task when a device gets data, it would sleep until a signal
        // happened to arrive. give up the cpu and look again next time round, same as a pipe
        sched_yield(current);

        if (signal_should_interrupt(current))
            return -EINTR;
    }
}

long vfs_write(int fd, const void *buf, size_t count){
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return -EBADF;

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops)
        return -EBADF;

    uintptr_t inode_addr = (uintptr_t)f->inode;
    uintptr_t ops_addr   = (uintptr_t)f->inode->ops;
    if (inode_addr < 0xFFFF800000000000ULL || ops_addr < 0xFFFF800000000000ULL)
        return -EBADF;

    int mode = f->flags & O_MODE;
    if (mode != O_WRONLY && mode != O_RDWR)
        return -EBADF;

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
        return -ENOSYS;
    }

    r = parent_inode->ops->unlink(parent_inode, name, namelen);
    if (r >= 0) {
        // still open somewhere so its still alive, but nobody should find it by name anymore
        INode_t *victim = dcache_find(parent_inode, name, namelen);
        if (victim) dcache_remove(victim);
    }

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
        return -ENOSYS;
    }

    r = old_parent->ops->rename(old_parent, oldname, oldlen, newname, newlen);
    if (r >= 0) {
        // the cache is keyed on the name, move the entry over to the new one
        INode_t *moved = dcache_find(old_parent, oldname, oldlen);
        if (moved && newlen < sizeof(moved->name)) {
            dcache_remove(moved);
            memcpy(moved->name, newname, newlen);
            moved->name[newlen] = '\0';
            dcache_insert(moved);
        } else if (moved) {
            dcache_remove(moved);
        }
    }

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
        return -EINVAL;

    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table)
        return -EMFILE;

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
        return -EMFILE;

    file_t *read_file = NULL;
    file_t *write_file = NULL;
    if (pipe_create_file_pair(&read_file, &write_file) != 0)
        return -ENOMEM;

    fd_table->fds[read_fd] = read_file;
    fd_table->fds[write_fd] = write_file;
    out_fds[0] = read_fd;
    out_fds[1] = write_fd;
    return 0;
}

int vfs_dup2(int oldfd, int newfd) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return -EBADF;

    file_t *src = fd_table->fds[oldfd];
    if (!src)
        return -EBADF;

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
    if (!fd_table || fd < 0 || fd >= MAX_FDS) return -EBADF;

    file_t *f = fd_table->fds[fd];
    if (!f) return -EBADF;

    fd_table->fds[fd] = NULL;
    vfs_file_put(f);

    return 0;
}

long vfs_seek(int fd, long offset, int whence) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return -EBADF;

    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode)
        return -EBADF;

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
            return -EINVAL;
    }

    if (new_offset < 0)
        return -EINVAL;

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
    if (!parent || !parent->ops || !parent->ops->create) return -ENOSYS;
    return parent->ops->create(parent, name, namelen, result, node_type);
}

int inode_lookup(INode_t* dir, const char* name, size_t name_len, INode_t** result){
    if (!dir || !dir->ops || !dir->ops->lookup) return -ENOSYS;
    return dir->ops->lookup(dir, name, name_len, result);
}

void inode_drop(INode_t* inode){
    if (!inode || !inode->ops || !inode->ops->drop) return;
    inode->ops->drop(inode);
}

int inode_truncate(INode_t* inode, size_t new_size){
    if (!inode || !inode->ops || !inode->ops->truncate) return -ENOSYS;
    return inode->ops->truncate(inode, new_size);
}

long inode_write(INode_t* inode, const void* in_buffer, size_t count, size_t offset){
    if (!inode || !inode->ops || !inode->ops->write) return -ENOSYS;
    return inode->ops->write(inode, in_buffer, count, offset);
}

long inode_read(INode_t* inode, void* out_buffer, size_t count, size_t offset){
    if (!inode || !inode->ops || !inode->ops->read) return -ENOSYS;
    return inode->ops->read(inode, out_buffer, count, offset);
}

int vfs_readdir(INode_t* dir, size_t index, INode_t** result){
    if(!dir || !dir->ops || !dir->ops->readdir) return -ENOSYS;

    int r = dir->ops->readdir(dir, index, result);
    if (r < 0 || !*result) return r;

    // a filesystem that builds inodes on the fly just made a new one, if we already know
    // this name hand back the one everybody else is holding
    *result = vfs_adopt(dir, *result, (*result)->name, strlen((*result)->name));
    return 0;
}
