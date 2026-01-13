#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <user/user_copy.h>

int sys_open(char *path_str, int flags) {
    char *kpath = from_user_copy_string((const char *)path_str, 256);
    if (!kpath) return -1;

    int fd = vfs_open(kpath, flags);

    return fd;
}
