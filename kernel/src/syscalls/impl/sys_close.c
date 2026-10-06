#include <fs/vfs.h>
#include <user/errno.h>

int sys_close(int fd){
    return vfs_close(fd);
}
