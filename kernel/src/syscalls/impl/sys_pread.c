#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>
#include <sched/scheduler.h>
#include <syscalls/syscall.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define PIO_CHUNK PAGE_SIZE

static long positional_io(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t offset, int write) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (fd >= MAX_FDS || !caller->fd_table || !caller->fd_table->fds[fd])
        return -EBADF;
    if ((int64_t)offset < 0)
        return -EINVAL;
    if (len == 0)
        return 0;
    if (!user_buf)
        return -EFAULT;

    char *kbuf = kmalloc(PIO_CHUNK);
    if (!kbuf)
        return -ENOMEM;

    uint64_t done = 0;
    long ret = 0;
    while (done < len) {
        size_t n = len - done > PIO_CHUNK ? PIO_CHUNK : len - done;
        long r;
        if (write) {
            if (copy_from_user(caller, kbuf, (const void *)(user_buf + done), n) != 0) {
                ret = -EFAULT;
                break;
            }
            r = vfs_pwrite((int)fd, kbuf, n, offset + done);
        } else {
            r = vfs_pread((int)fd, kbuf, n, offset + done);
            if (r > 0 && copy_to_user(caller, (void *)(user_buf + done), kbuf, (size_t)r) != 0) {
                ret = -EFAULT;
                break;
            }
        }
        if (r < 0) {
            ret = r;
            break;
        }
        done += (uint64_t)r;
        if ((size_t)r < n)
            break;
    }
    kfree(kbuf);
    return done ? (long)done : ret;
}

long sys_pread64(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t offset) {
    return positional_io(fd, user_buf, len, offset, 0);
}

long sys_pwrite64(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t offset) {
    return positional_io(fd, user_buf, len, offset, 1);
}
