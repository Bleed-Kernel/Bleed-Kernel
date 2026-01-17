#include <fs/vfs.h>
#include <stdint.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len) {
    if (fd >= MAX_FDS || !current_fd_table) return -1;

    file_t* f = current_fd_table->fds[fd];
    if (!f || ((f->flags & O_RDONLY) == 0 && (f->flags & O_RDWR) == 0)) return -1;

    long r = inode_read(f->inode, (void*)user_buf, len, f->offset);
    if (r > 0) f->offset += r;

    if (r < 0 && f->offset >= vfs_filesize(f->inode)) return 0;

    return r;
}
