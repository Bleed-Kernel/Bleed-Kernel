#include <fs/vfs.h>
#include <user/errno.h>

int sys_close(int fd){
    int rc = vfs_close(fd);
    if (rc < 0)
        return -EBADF;
    return 0;
}
