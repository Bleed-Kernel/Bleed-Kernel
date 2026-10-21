#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>
#include <mm/smap.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define SYS_WRITE_CHUNK (64 * 1024)
#define SYS_WRITE_SMALL 256

uint64_t sys_write(uint64_t fd, uint64_t user_buf, uint64_t len) {
    task_t *caller = get_current_task();
    if (!caller)
        return (uint64_t)-ESRCH;
    if (fd >= MAX_FDS || !caller->fd_table)
        return (uint64_t)-EBADF;
    if (len == 0)
        return 0;
    if (!user_buf)
        return (uint64_t)-EFAULT;

    file_t* f = caller->fd_table->fds[fd];
    if (!f)
        return (uint64_t)-EBADF;

    int mode = f->flags & O_MODE;
    if (mode != O_WRONLY && mode != O_RDWR)
        return (uint64_t)-EBADF;

    int direct = f->inode && f->inode->type == INODE_FILE;

    size_t chunk = len < SYS_WRITE_CHUNK ? len : SYS_WRITE_CHUNK;
    char  small[SYS_WRITE_SMALL];
    char* kbuf = NULL;

    // pipes and terminals mostly get a few bytes at a time, not worth a kmalloc
    if (chunk <= sizeof(small))
        kbuf = small;

    uint64_t total = 0;
    while (total < len) {
        uintptr_t at = user_buf + total;
        const void *src = NULL;

        size_t batch = len - total;
        if (direct) {
            size_t to_page_end = PAGE_SIZE - (at & (PAGE_SIZE - 1));
            if (batch > to_page_end)
                batch = to_page_end;
            src = user_page_ptr(caller, at, 0);
        }

        if (!src) {
            if (batch > chunk)
                batch = chunk;
            if (!kbuf && !(kbuf = kmalloc(chunk)))
                return total ? total : (uint64_t)-ENOMEM;
            if (copy_from_user(caller, kbuf, (const void *)at, batch) != 0) {
                if (kbuf != small) kfree(kbuf);
                return total ? total : (uint64_t)-EFAULT;
            }
            src = kbuf;
        }

        long written = inode_write(f->inode, src, batch, f->offset);
        if (written < 0) {
            if (kbuf != small) kfree(kbuf);
            return total ? total : (uint64_t)written;
        }

        f->offset += written;
        total += (uint64_t)written;

        // short write, the caller can come back for the rest
        if ((size_t)written < batch)
            break;
    }

    if (kbuf != small) kfree(kbuf);
    return total;
}
