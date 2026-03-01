#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>
#include <mm/smap.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

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

    char* kbuf = kmalloc(len);
    if (!kbuf)
        return (uint64_t)-ENOMEM;

    if (copy_from_user(caller, kbuf, (const void *)user_buf, len) != 0) {
        kfree(kbuf, len);
        return (uint64_t)-EFAULT;
    }
    long written = inode_write(f->inode, kbuf, len, f->offset);
    if (written > 0)
        f->offset += written;

    kfree(kbuf, len);
    if (written < 0)
        return (uint64_t)-EIO;
    return written;
}
