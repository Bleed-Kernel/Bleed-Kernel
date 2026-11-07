#include <fs/vfs.h>
#include <user/errno.h>

long sys_close(int fd){
    return vfs_close(fd);
}
