#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>
#include <mm/smap.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define SYS_WRITE_CHUNK (64 * 1024)

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

    // len is whatever userspace says it is, bounce it through a bounded buffer instead of
    // letting one write size a kernel allocation
    size_t chunk = len < SYS_WRITE_CHUNK ? len : SYS_WRITE_CHUNK;
    char* kbuf = kmalloc(chunk);
    if (!kbuf)
        return (uint64_t)-ENOMEM;

    uint64_t total = 0;
    while (total < len) {
        size_t batch = len - total;
        if (batch > chunk)
            batch = chunk;

        if (copy_from_user(caller, kbuf, (const void *)(user_buf + total), batch) != 0) {
            kfree(kbuf);
            return total ? total : (uint64_t)-EFAULT;
        }

        long written = inode_write(f->inode, kbuf, batch, f->offset);
        if (written < 0) {
            kfree(kbuf);
            return total ? total : (uint64_t)written;
        }

        f->offset += written;
        total += (uint64_t)written;

        // short write, the caller can come back for the rest
        if ((size_t)written < batch)
            break;
    }

    kfree(kbuf);
    return total;
}
