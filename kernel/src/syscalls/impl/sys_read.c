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

    char kbuf[512]; 
    size_t batch_size = (len > sizeof(kbuf)) ? sizeof(kbuf) : len;

    long total_read = vfs_read((int)fd, kbuf, batch_size);
    
    if (total_read > 0) {
        memcpy((void *)user_buf, kbuf, total_read);
    }

    return (uint64_t)total_read;
}