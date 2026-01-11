#include <fs/vfs.h>

int sys_close(int fd){
    return vfs_close(fd);
}