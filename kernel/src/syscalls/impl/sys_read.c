#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (fd >= MAX_FDS || !current_fd_table)
        return -1;

    file_t *f = current_fd_table->fds[fd];
    if (!f)
        return -1;

    int mode = f->flags & O_MODE;
    if (mode != O_RDONLY && mode != O_RDWR)
        return -1;

    char *kbuf = kmalloc(len);
    if (!kbuf)
        return -1;

    long r = inode_read(f->inode, kbuf, len, f->offset);
    if (r > 0) {
        memcpy((void *)user_buf, kbuf, r);
        f->offset += r;
    }

    kfree(kbuf, len);
    return r;
}
