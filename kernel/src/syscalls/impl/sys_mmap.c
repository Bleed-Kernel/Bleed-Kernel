#include <mm/userspace/mmap.h>
#include <mm/paging.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <fs/vfs.h>
#include <fs/memfd.h>

// page aligned length, 0 if it doesnt fit
static uint64_t vma_len(uint64_t addr, uint64_t len) {
    uint64_t end;
    if (!len || __builtin_add_overflow(addr, len, &end) || end > PAGE_ALIGN_DOWN(UINT64_MAX))
        return 0;
    return PAGE_ALIGN_UP(len);
}

static long map_fd(task_t *task, uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off) {
    if (off & (PAGE_SIZE - 1))
        return -EINVAL;

    file_t *f = vfs_file_get(fd);
    if (!f)
        return -EBADF;

    long ret;
    int  mode   = f->flags & O_MODE;
    bool shared = (flags & MAP_TYPE) != MAP_PRIVATE;
    if (f->type != FD_TYPE_FS || !f->inode || f->inode->type != INODE_FILE)
        ret = -ENODEV;
    else if (mode == O_WRONLY || (shared && (prot & PROT_WRITE) && mode != O_RDWR))
        ret = -EACCES;
    // TODO: writing back to a real file needs a page cache, read only shared is a private copy
    else if (shared && (prot & PROT_WRITE) && !memfd_is(f->inode))
        ret = -ENODEV;
    else
        ret = task_vma_map_file(task, addr, len, prot,
                                (flags & ~MAP_KERNEL_NOWRITE) | (shared && mode != O_RDWR ? MAP_KERNEL_NOWRITE : 0),
                                f->inode, off);

    vfs_file_put(f);
    return ret;
}

long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;

    len = vma_len(addr, len);
    if (!len)
        return -EINVAL;
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;

    uint64_t type = flags & MAP_TYPE;
    if (type != MAP_PRIVATE && type != MAP_SHARED && type != MAP_SHARED_VALIDATE)
        return -EINVAL;

    if (!(flags & MAP_ANONYMOUS))
        return map_fd(task, addr, len, (int)prot, (int)flags, (int)fd, off);
    return task_vma_map(task, addr, len, (int)prot, (int)flags);
}

// len 0 is ours, linux says EINVAL. it unmaps the whole mapping at addr, for the old
// callers that only ever kept the address (ipc, shm, audio rings)
long sys_munmap(uint64_t addr, uint64_t len) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;

    if (!len) {
        task_munmap(task, (void *)addr);
        return 0;
    }

    len = vma_len(addr, len);
    if (!len || !user_ptr_valid(addr))
        return -EINVAL;
    return task_vma_unmap(task, addr, len);
}

long sys_madvise(uint64_t addr, uint64_t len, uint64_t advice) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (!len)
        return 0;

    len = vma_len(addr, len);
    if (!len)
        return -EINVAL;

    // the rest are only hints
    if (advice == MADV_DONTNEED || advice == MADV_FREE)
        return task_vma_discard(task, addr, len);
    return 0;
}
