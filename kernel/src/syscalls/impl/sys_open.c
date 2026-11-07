#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <user/user_copy.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/errno.h>

static int open_common(int dirfd, char *path_str, int flags) {
    if (!path_str)
        return -EFAULT;

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    long plen = copy_user_path(caller, path_str, kpath, sizeof(kpath));
    if (plen == -2)
        return -E2BIG;
    if (plen < 0)
        return -EFAULT;

    int fd = vfs_open_at(dirfd, kpath, flags & ~O_CLOEXEC);
    if (fd >= 0 && (flags & O_CLOEXEC))
        vfs_set_fd_flags(fd, FD_CLOEXEC);
    return fd;
}

long sys_open(char *path_str, int flags) {
    return open_common(AT_FDCWD, path_str, flags);
}

long sys_openat(int dirfd, char *path_str, int flags) {
    return open_common(dirfd, path_str, flags);
}
