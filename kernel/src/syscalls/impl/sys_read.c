#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len) {
    task_t *caller = get_current_task();
    if (!caller)
        return (uint64_t)-ESRCH;
    if (fd >= MAX_FDS || !caller->fd_table)
        return (uint64_t)-EBADF;

    file_t *f = caller->fd_table->fds[fd];
    if (!f)
        return (uint64_t)-EBADF;

    if (len == 0)
        return 0;
    if (!user_buf)
        return (uint64_t)-EFAULT;

    // a page off the heap when it can, the stack one is the fallback
    char     small[256];
    int      direct = f->inode && f->inode->type == INODE_FILE;
    char    *kbuf   = direct ? NULL : kmalloc(len < PAGE_SIZE ? len : PAGE_SIZE);
    size_t   kcap   = kbuf ? (len < PAGE_SIZE ? len : PAGE_SIZE) : sizeof(small);
    uint64_t copied = 0;
    uint64_t ret;

    if (!kbuf)
        kbuf = small;

    while (copied < len) {
        uintptr_t at = user_buf + copied;
        void *dst = NULL;

        size_t batch_size = len - copied;
        if (direct) {
            size_t to_page_end = PAGE_SIZE - (at & (PAGE_SIZE - 1));
            if (batch_size > to_page_end)
                batch_size = to_page_end;
            dst = user_page_ptr(caller, at, 1);
        }
        if (!dst && batch_size > kcap)
            batch_size = kcap;

        long r = vfs_read((int)fd, dst ? dst : kbuf, batch_size);
        if (r < 0) {
            ret = copied ? copied : (uint64_t)r;
            goto out;
        }
        if (r == 0)
            break;

        if (!dst && copy_to_user(caller, (void *)at, kbuf, (size_t)r) != 0) {
            ret = copied ? copied : (uint64_t)-EFAULT;
            goto out;
        }
        copied += (uint64_t)r;

        if ((size_t)r < batch_size)
            break;
    }
    ret = copied;

out:
    if (kbuf != small)
        kfree(kbuf);
    return ret;
}
