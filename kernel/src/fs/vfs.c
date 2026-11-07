#include <fs/vfs.h>
#include <user/user_poll.h>
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
#include <user/user_stat.h>
#include <user/errno.h>
#include <mm/spinlock.h>
#include <devices/devices.h>
#include <fs/pipe.h>
#include <net/socket.h>
#include <net/unix.h>
#include <fs/epoll.h>

extern const filesystem tempfs;

static fd_table_t *boot_fd_table = NULL;

INode_t* vfs_root = NULL;

static fd_table_t *vfs_fd_table_alloc(void) {
    fd_table_t *table = kmalloc(sizeof(fd_table_t));
    if (!table)
        return NULL;
    memset(table, 0, sizeof(fd_table_t));
    table->refs = 1;

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
        dst->fds[fd]      = f;
        dst->fd_flags[fd] = src->fd_flags[fd];
        if (f)
            f->shared++;
    }

    return dst;
}

// let go of one fd's hold on a file, the last one out releases whats behind it
void vfs_file_put(file_t *f) {
    f->shared--;
    if (f->shared > 0)
        return;

    if (f->type == FD_TYPE_PIPE)
        pipe_file_release(f);
    else if (f->type == FD_TYPE_SOCKET)
        socket_file_release(f);
    else if (f->type == FD_TYPE_UNIX)
        unix_file_release(f);
    else if (f->type == FD_TYPE_EPOLL)
        epoll_file_release(f);
    else {
        if (f->inode && f->inode->ops && f->inode->ops->file_release)
            f->inode->ops->file_release(f);
        vfs_drop(f->inode);
    }
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

fd_table_t *vfs_fd_table_share(fd_table_t *table) {
    if (table)
        table->refs++;
    return table;
}

void vfs_fd_table_drop(fd_table_t *table) {
    if (!table)
        return;
    if (--table->refs > 0)
        return;

    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_t *f = table->fds[fd];
        if (!f)
            continue;

        table->fds[fd]      = NULL;
        table->fd_flags[fd] = 0;
        vfs_file_put(f);
    }

    if (table == boot_fd_table)
        boot_fd_table = NULL;

    kfree(table);
}

INode_t* vfs_get_root(void){
    return vfs_root;
}

int vfs_inode_path(INode_t *inode, char *out, size_t size){
    if (!inode || !out || size < 2) return -1;

    // the names are walked up from the inode, so it goes in backwards from the end of out
    size_t pos = size - 1;
    out[pos] = '\0';

    for (INode_t *cur = inode; cur != vfs_root; cur = cur->parent) {
        // no path to give it fell out
        if (!cur || !cur->name[0]) return -1;

        size_t len = strlen(cur->name);
        if (len + 1 > pos) return -1;

        pos -= len;
        memcpy(out + pos, cur->name, len);
        out[--pos] = '/';
    }

    if (pos == size - 1) out[--pos] = '/';
    memmove(out, out + pos, size - pos);
    return 0;
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
    inode->dcache_next   = dcache[b];
    inode->dcache_bucket = (uint16_t)b;
    dcache[b] = inode;
    inode->hashed = 1;
}

static void dcache_remove(INode_t *inode) {
    if (!inode->hashed) return;

    // the bucket it went in, not one worked out from the name now
    INode_t **link = &dcache[inode->dcache_bucket];
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

static int vfs_walk(const path_t* path, INode_t** out_inode, int follow_last, int depth);

// where link (in dir) points, as a ref the caller owns
static int vfs_follow(INode_t *dir, INode_t *link, INode_t **out, int depth) {
    if (depth >= VFS_LINK_DEPTH)
        return -ELOOP;
    if (!link->ops || !link->ops->readlink)
        return -EIO;

    char *target = kmalloc(VFS_LINK_MAX);
    if (!target)
        return -ENOMEM;
    long n = link->ops->readlink(link, target, VFS_LINK_MAX - 1);
    if (n <= 0) {
        kfree(target);
        return n < 0 ? (int)n : -ENOENT;
    }
    target[n] = '\0';

    // relative targets start from the folder the link is in
    path_t p = {
        .root        = vfs_root,
        .start       = target[0] == '/' ? vfs_root : dir,
        .data        = target,
        .data_length = (size_t)n,
    };
    int r = vfs_walk(&p, out, 1, depth + 1);
    kfree(target);
    return r;
}

int vfs_lookup(const path_t* path, INode_t** out_inode){
    return vfs_walk(path, out_inode, 1, 0);
}

int vfs_lookup_nofollow(const path_t* path, INode_t** out_inode){
    return vfs_walk(path, out_inode, 0, 0);
}

static int vfs_walk(const path_t* path, INode_t** out_inode, int follow_last, int depth){
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

        // links in the middle are always followed, the last one only when asked. a trailing
        // slash counts as more path like it does on linux
        if (next->type == INODE_SYMLINK && (head < head_end || follow_last)) {
            INode_t *to = NULL;
            int r = vfs_follow(cur, next, &to, depth);
            vfs_drop(next);
            if (r < 0) {
                vfs_drop(cur);
                return r;
            }
            next = to;
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

    if (inode->ops && inode->ops->file_ioctl)
        return inode->ops->file_ioctl(file, request, arg);
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

static int vfs_at_base(int dirfd, INode_t **out) {
    if (dirfd == AT_FDCWD) {
        task_t *task = get_current_task();
        *out = task ? task->current_directory : NULL;
        return 0;
    }

    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || dirfd < 0 || dirfd >= MAX_FDS || !fd_table->fds[dirfd])
        return -EBADF;

    file_t *f = fd_table->fds[dirfd];
    if (f->type == FD_TYPE_PIPE || !f->inode)
        return -ENOTDIR;
    if (f->inode->type != INODE_DIRECTORY)
        return -ENOTDIR;

    *out = f->inode;
    return 0;
}

int vfs_open(const char *path_str, int flags){
    return vfs_open_at(AT_FDCWD, path_str, flags);
}

// linux's /proc/self/fd/N, a new open file on whatever fd N has open
static int vfs_reopen_fd(fd_table_t *fd_table, const char *num, int flags) {
    int n = 0;
    if (!*num)
        return -ENOENT;
    for (; *num; num++) {
        if (*num < '0' || *num > '9' || n > MAX_FDS)
            return -ENOENT;
        n = n * 10 + (*num - '0');
    }
    file_t *src = n < MAX_FDS ? fd_table->fds[n] : NULL;
    if (!src)
        return -ENOENT;
    if ((src->type != FD_TYPE_FS && src->type != FD_TYPE_DEV) || !src->inode)
        return -ENXIO;

    int want = flags & O_MODE, have = src->flags & O_MODE;
    if ((want != O_RDONLY && have == O_RDONLY) || (want != O_WRONLY && have == O_WRONLY))
        return -EACCES;

    file_t *f = kmalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    vfs_hold(src->inode);
    f->inode  = src->inode;
    f->type   = src->type;
    f->offset = 0;
    f->flags  = flags & (O_MODE | O_APPEND);
    f->shared = 1;

    int fd = vfs_fd_install(fd_table, f);
    if (fd >= 0)
        return fd;
    kfree(f);
    vfs_drop(src->inode);
    return -EMFILE;
}

int vfs_open_at(int dirfd, const char *path_str, int flags){
    if (!path_str || path_str[0] == '\0')
        return -ENOENT;
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table) return -EMFILE;

    if (!strncmp(path_str, "/proc/self/fd/", 14))
        return vfs_reopen_fd(fd_table, path_str + 14, flags);

    INode_t *base = NULL;
    if (path_str[0] != '/') {
        int b = vfs_at_base(dirfd, &base);
        if (b < 0) return b;
    }
    path_t path = vfs_path_from_relative(path_str, base);
    INode_t *inode = NULL;

    int l = (flags & O_NOFOLLOW) ? vfs_lookup_nofollow(&path, &inode) : vfs_lookup(&path, &inode);
    if (l == 0 && inode->type == INODE_SYMLINK) {
        // only O_NOFOLLOW hands one back, and a link cant be opened
        vfs_drop(inode);
        return -ELOOP;
    }
    if (l < 0){
        if (!(flags & O_CREAT)) return l;
        l = vfs_create(&path, &inode, INODE_FILE);
        if (l < 0) return l;
        // do not bump here! the file structure takes over the callers ref
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        vfs_drop(inode);
        return -EEXIST;
    }

    if ((flags & O_DIRECTORY) && inode->type != INODE_DIRECTORY) {
        vfs_drop(inode);
        return -ENOTDIR;
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
        task_t  *current = get_current_task();
        sigset_t seen    = current ? current->sig_pending : 0;
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

        if (f->type == FD_TYPE_PIPE || f->type == FD_TYPE_SOCKET || f->type == FD_TYPE_UNIX)
            return 0;

        if (is_size_finite)
            return 0;

        // sleep till the device says theres something. one without a queue gets looked at next turn
        const INodeOps_t *ops   = f->inode->ops;
        wait_queue_t     *queue = ops->wait_queue && ops->poll ? ops->wait_queue(f->inode) : NULL;
        if (!queue || !current) {
            sched_yield(current);
        } else {
            unsigned long flags = irq_push();
            if (!(ops->poll(f->inode) & (POLLIN | POLLHUP | POLLERR)) && current->sig_pending == seen)
                sched_wait_queue(queue, 0);
            irq_restore(flags);
        }

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

// pread and pwrite, only for real files and the fds offset stays where it was. threads!!!
static file_t *vfs_positional_file(int fd, int want_write) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS)
        return NULL;
    file_t *f = fd_table->fds[fd];
    if (!f || !f->inode || !f->inode->ops)
        return NULL;
    if ((uintptr_t)f->inode < 0xFFFF800000000000ULL || (uintptr_t)f->inode->ops < 0xFFFF800000000000ULL)
        return NULL;
    int mode = f->flags & O_MODE;
    if (want_write ? (mode != O_WRONLY && mode != O_RDWR) : (mode != O_RDONLY && mode != O_RDWR))
        return NULL;
    return f;
}

long vfs_pread(int fd, void *buf, size_t count, size_t offset) {
    file_t *f = vfs_positional_file(fd, 0);
    if (!f)
        return -EBADF;
    if (f->type != FD_TYPE_FS || f->inode->type != INODE_FILE)
        return -ESPIPE;
    if (!f->inode->internal_data)
        return -EBADF;

    uint64_t filesize = f->inode->ops->size ? f->inode->ops->size(f->inode) : vfs_filesize(f->inode);
    if (offset >= filesize)
        return 0;
    if (count > filesize - offset)
        count = filesize - offset;
    return inode_read(f->inode, buf, count, offset);
}

long vfs_pwrite(int fd, const void *buf, size_t count, size_t offset) {
    file_t *f = vfs_positional_file(fd, 1);
    if (!f)
        return -EBADF;
    if (f->type != FD_TYPE_FS || f->inode->type != INODE_FILE)
        return -ESPIPE;
    return inode_write(f->inode, buf, count, offset);
}

static int vfs_unlink_from(INode_t *cwd, const char *path_str);

int vfs_unlink(const char *path_str) {
    task_t *task = get_current_task();
    return vfs_unlink_from(task ? task->current_directory : NULL, path_str);
}

int vfs_unlink_at(int dirfd, const char *path_str) {
    INode_t *base = NULL;
    int r = vfs_at_base(dirfd, &base);
    if (r < 0)
        return r;
    return vfs_unlink_from(base, path_str);
}

static int vfs_unlink_from(INode_t *cwd, const char *path_str) {
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

// rename onto a name thats taken replaces it, thats how everything saves a file safely
static int vfs_rename_over(INode_t *parent, const char *oldpath, const char *newpath, INode_t *cwd,
                           const char *newname, size_t newlen) {
    INode_t *from = NULL, *to = NULL;
    path_t fp = vfs_path_from_relative(oldpath, cwd);
    path_t tp = vfs_path_from_relative(newpath, cwd);
    int r = vfs_lookup_nofollow(&fp, &from);
    if (r < 0)
        return r;
    r = vfs_lookup_nofollow(&tp, &to);
    if (r < 0) {
        vfs_drop(from);
        return -EEXIST;
    }

    if (from == to)
        r = 0;
    else if (to->type == INODE_DIRECTORY)
        r = from->type == INODE_DIRECTORY ? -ENOTEMPTY : -EISDIR;
    else if (from->type == INODE_DIRECTORY)
        r = -ENOTDIR;
    else if (!parent->ops->unlink)
        r = -EEXIST;
    else
        r = 1;
    vfs_drop(from);
    vfs_drop(to);
    if (r != 1)
        return r;

    r = parent->ops->unlink(parent, newname, newlen);
    if (r < 0)
        return r;
    INode_t *victim = dcache_find(parent, newname, newlen);
    if (victim)
        dcache_remove(victim);
    return 1;
}

// the filesystem rename with the cache entry moved across
static int vfs_rename_entry(INode_t *parent, const char *oldname, size_t oldlen,
                            const char *newname, size_t newlen) {
    INode_t *moved = dcache_find(parent, oldname, oldlen);
    if (moved)
        dcache_remove(moved);

    int r = parent->ops->rename(parent, oldname, oldlen, newname, newlen);
    if (moved && r >= 0 && newlen < sizeof(moved->name)) {
        memcpy(moved->name, newname, newlen);
        moved->name[newlen] = '\0';
        dcache_insert(moved);
    } else if (moved && r < 0) {
        dcache_insert(moved);
    }
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

    r = vfs_rename_entry(old_parent, oldname, oldlen, newname, newlen);
    if (r == -EEXIST) {
        r = vfs_rename_over(old_parent, oldpath, newpath, cwd, newname, newlen);
        if (r == 1)
            r = vfs_rename_entry(old_parent, oldname, oldlen, newname, newlen);
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

file_t *vfs_file_get(int fd) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS || !fd_table->fds[fd])
        return NULL;
    file_t *f = fd_table->fds[fd];
    f->shared++;
    return f;
}

int vfs_install(file_t *f) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table)
        return -EMFILE;

    // same as vfs_pipe avoiding my reserves
    for (int fd = 3; fd < MAX_FDS; fd++) {
        if (!fd_table->fds[fd]) {
            fd_table->fds[fd] = f;
            return fd;
        }
    }
    return -EMFILE;
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
    fd_table->fds[newfd]      = src;
    fd_table->fd_flags[newfd] = 0;
    return newfd;
}

int vfs_dup_from(int oldfd, int lowest) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || oldfd < 0 || oldfd >= MAX_FDS || !fd_table->fds[oldfd])
        return -EBADF;
    if (lowest < 0 || lowest >= MAX_FDS)
        return -EINVAL;

    for (int fd = lowest; fd < MAX_FDS; fd++) {
        if (!fd_table->fds[fd])
            return vfs_dup2(oldfd, fd);
    }
    return -EMFILE;
}

int vfs_get_fd_flags(int fd) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS || !fd_table->fds[fd])
        return -EBADF;
    return fd_table->fd_flags[fd];
}

int vfs_set_fd_flags(int fd, int flags) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS || !fd_table->fds[fd])
        return -EBADF;
    fd_table->fd_flags[fd] = (uint8_t)(flags & FD_CLOEXEC);
    return 0;
}

int vfs_set_nonblock(int fd) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS || !fd_table->fds[fd])
        return -EBADF;
    file_t *f = fd_table->fds[fd];
    if (f->type != FD_TYPE_PIPE)
        return -ENOTTY;
    pipe_set_nonblock(f);
    return 0;
}

void vfs_close_on_exec(fd_table_t *table) {
    if (!table)
        return;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        file_t *f = table->fds[fd];
        if (!f || !(table->fd_flags[fd] & FD_CLOEXEC))
            continue;
        table->fds[fd]      = NULL;
        table->fd_flags[fd] = 0;
        vfs_file_put(f);
    }
}

int vfs_dup(int oldfd) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || oldfd < 0 || oldfd >= MAX_FDS || !fd_table->fds[oldfd])
        return -EBADF;

    // the lowest free one, like everywhere else
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (!fd_table->fds[fd])
            return vfs_dup2(oldfd, fd);
    }
    return -EMFILE;
}

int vfs_close(int fd){
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS) return -EBADF;

    file_t *f = fd_table->fds[fd];
    if (!f) return -EBADF;

    fd_table->fds[fd]      = NULL;
    fd_table->fd_flags[fd] = 0;
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

// theres no owners or permissions yet so everything is open to everyone
static void stat_fill(INode_t *inode, user_stat_t *st) {
    memset(st, 0, sizeof(*st));
    st->dev     = 1;
    st->ino     = (uint64_t)(uintptr_t)inode;
    st->nlink   = 1;
    st->blksize = 4096;

    switch (inode->type) {
    case INODE_DIRECTORY:
        st->mode  = STAT_IFDIR | 0755;
        st->nlink = 2;
        break;
    case INODE_DEVICE:
        st->mode = STAT_IFCHR | 0666;
        break;
    case INODE_SYMLINK: {
        char target[VFS_LINK_MAX];
        long n = inode->ops && inode->ops->readlink ? inode->ops->readlink(inode, target, sizeof(target)) : 0;
        st->mode = STAT_IFLNK | 0777;
        st->size = n > 0 ? n : 0;
        break;
    }
    default:
        st->mode = STAT_IFREG | 0755;
        st->size = (int64_t)vfs_filesize(inode);
        break;
    }
    st->blocks = (st->size + 511) / 512;

    if (inode->ops && inode->ops->times)
        inode->ops->times(inode, &st->atime, &st->mtime, &st->ctime);
}

int vfs_stat(const char *path_str, user_stat_t *st) {
    return vfs_stat_at(AT_FDCWD, path_str, st);
}

static int vfs_stat_flags(int dirfd, const char *path_str, user_stat_t *st, int follow) {
    if (!path_str || !path_str[0])
        return -ENOENT;

    INode_t *base = NULL;
    if (path_str[0] != '/') {
        int b = vfs_at_base(dirfd, &base);
        if (b < 0) return b;
    }
    path_t   path  = vfs_path_from_relative(path_str, base);
    INode_t *inode = NULL;
    int      r     = follow ? vfs_lookup(&path, &inode) : vfs_lookup_nofollow(&path, &inode);
    if (r < 0)
        return r;

    stat_fill(inode, st);
    vfs_drop(inode);
    return 0;
}

int vfs_stat_at(int dirfd, const char *path_str, user_stat_t *st) {
    return vfs_stat_flags(dirfd, path_str, st, 1);
}

int vfs_lstat_at(int dirfd, const char *path_str, user_stat_t *st) {
    return vfs_stat_flags(dirfd, path_str, st, 0);
}

int vfs_symlink_at(const char *target, int dirfd, const char *path_str) {
    if (!target || !target[0] || strlen(target) >= VFS_LINK_MAX)
        return target && target[0] ? -ENAMETOOLONG : -ENOENT;

    INode_t *base = NULL;
    if (path_str[0] != '/') {
        int b = vfs_at_base(dirfd, &base);
        if (b < 0) return b;
    }

    path_t path = vfs_path_from_relative(path_str, base);
    INode_t *there = NULL;
    if (vfs_lookup_nofollow(&path, &there) == 0) {
        vfs_drop(there);
        return -EEXIST;
    }

    path_t parent;
    const char *name;
    size_t namelen;
    int r = vfs_path_split(&path, &parent, &name, &namelen);
    if (r < 0)
        return r;

    INode_t *dir = NULL;
    r = vfs_lookup(&parent, &dir);
    if (r < 0)
        return r;
    if (dir->type != INODE_DIRECTORY) {
        vfs_drop(dir);
        return -ENOTDIR;
    }
    // fat and exfat have nowhere to keep one
    if (!dir->ops || !dir->ops->symlink) {
        vfs_drop(dir);
        return -EPERM;
    }

    INode_t *link = NULL;
    r = dir->ops->symlink(dir, name, namelen, target, &link);
    if (r == 0 && link) {
        vfs_attach(link, dir, name, namelen);
        vfs_drop(link);
    }
    vfs_drop(dir);
    return r;
}

long vfs_readlink_at(int dirfd, const char *path_str, char *out, size_t size) {
    if (!path_str || !path_str[0])
        return -ENOENT;

    INode_t *base = NULL;
    if (path_str[0] != '/') {
        int b = vfs_at_base(dirfd, &base);
        if (b < 0) return b;
    }
    path_t   path  = vfs_path_from_relative(path_str, base);
    INode_t *inode = NULL;
    int      r     = vfs_lookup_nofollow(&path, &inode);
    if (r < 0)
        return r;

    long n = -EINVAL;
    if (inode->type == INODE_SYMLINK && inode->ops && inode->ops->readlink)
        n = inode->ops->readlink(inode, out, size);
    vfs_drop(inode);
    return n;
}

int vfs_fstat(int fd, user_stat_t *st) {
    fd_table_t *fd_table = vfs_get_active_fd_table();
    if (!fd_table || fd < 0 || fd >= MAX_FDS || !fd_table->fds[fd])
        return -EBADF;

    file_t *f = fd_table->fds[fd];
    if (f->type == FD_TYPE_PIPE || f->type == FD_TYPE_SOCKET || f->type == FD_TYPE_UNIX || f->type == FD_TYPE_EPOLL) {
        memset(st, 0, sizeof(*st));
        st->mode    = (f->type == FD_TYPE_PIPE ? STAT_IFIFO : STAT_IFSOCK) | 0600;
        st->nlink   = 1;
        st->blksize = 4096;
        return 0;
    }
    if (!f->inode)
        return -EBADF;
    stat_fill(f->inode, st);
    return 0;
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

void vfs_chdir_root(struct task *task) {
    if (!task) return;

    INode_t *old = task->current_directory;
    task->current_directory = vfs_root;
    if (vfs_root)
        vfs_root->shared++;
    vfs_drop(old);
}
