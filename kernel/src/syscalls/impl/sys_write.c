#include <fs/vfs.h>
#include <stdint.h>
#include <mm/kalloc.h>
#include <string.h>

uint64_t sys_write(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (fd >= MAX_FDS || !current_fd_table || !user_buf || len == 0) return 0;

    file_t* f = current_fd_table->fds[fd];
    if (!f || !(f->flags & O_WRONLY || f->flags & O_RDWR)) return -1;

    char* kbuf = kmalloc(len);
    if (!kbuf) return -1;

    memcpy(kbuf, (void*)user_buf, len);
    long written = inode_write(f->inode, kbuf, len, f->offset);
    if (written > 0) f->offset += written;

    kfree(kbuf, len);
    return written;
}
