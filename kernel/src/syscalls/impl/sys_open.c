#include <fs/vfs.h>

int sys_open(const char *path_str, int flags){
    return vfs_open(path_str, flags);
}