#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <user/user_copy.h>
#include <sched/scheduler.h>
#include <string.h>
#include <status.h>
#include <user/errno.h>

int sys_open(char *path_str, int flags) {
    if (!path_str)
        return -EFAULT;

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    if (copy_from_user(caller, kpath, path_str, sizeof(kpath)) != 0)
        return -EFAULT;

    int fd = vfs_open(kpath, flags);
    if (fd >= 0)
        return fd;

    if (fd == -FILE_NOT_FOUND)
        return -ENOENT;
    if (fd == -OUT_OF_BOUNDS)
        return -EMFILE;

    return -EIO;
}
