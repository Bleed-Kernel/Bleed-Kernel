#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (fd >= MAX_FDS || !current_fd_table)
        return -1;

    file_t *f = current_fd_table->fds[fd];
    if (!f)
        return -2;

    if (len == 0)
        return 0;

    char kbuf[2048];
    uint64_t copied = 0;

    while (copied < len) {
        size_t batch_size = len - copied;
        if (batch_size > sizeof(kbuf))
            batch_size = sizeof(kbuf);

        long r = vfs_read((int)fd, kbuf, batch_size);
        if (r < 0)
            return copied ? copied : (uint64_t)r;
        if (r == 0)
            break;

        umemcpy((void *)(user_buf + copied), kbuf, (size_t)r);
        copied += (uint64_t)r;

        if ((size_t)r < batch_size)
            break;
    }

    return copied;
}