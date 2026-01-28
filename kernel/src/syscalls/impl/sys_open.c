#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <user/user_copy.h>

int sys_open(char *path_str, int flags) {
    char *kpath = str_copy_from_user((const char *)path_str, 256);
    if (!kpath) return -1;

    int fd = vfs_open(kpath, flags);

    return fd;
}
