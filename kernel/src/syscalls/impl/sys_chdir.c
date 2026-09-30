#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <status.h>
#include <mm/kalloc.h>
#include <string.h>
#include <user/errno.h>

long sys_chdir(const char *user_path) {
    if (!user_path)
        return -EFAULT;

    char kbuf[PATH_MAX];
    memset(kbuf, 0, PATH_MAX);

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    long plen = copy_user_path(caller, user_path, kbuf, PATH_MAX);
    if (plen == -2)
        return -E2BIG;
    if (plen < 0)
        return -EFAULT;

    int r = vfs_chdir(kbuf);
    if (r == 0)
        return 0;
    if (r == -FILE_NOT_FOUND)
        return -ENOENT;
    return -EIO;
}
