#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <user/user_copy.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/errno.h>

int sys_open(char *path_str, int flags) {
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

    return vfs_open(kpath, flags);
}
