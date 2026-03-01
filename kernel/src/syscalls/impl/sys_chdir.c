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

    if (copy_from_user(caller, kbuf, user_path, PATH_MAX) != 0)
        return -EFAULT;
    kbuf[PATH_MAX - 1] = '\0';

    size_t plen = 0;
    while (plen < PATH_MAX && kbuf[plen] != '\0')
        plen++;
    if (plen == PATH_MAX)
        return -E2BIG;

    int r = vfs_chdir(kbuf);
    if (r == 0)
        return 0;
    if (r == -FILE_NOT_FOUND)
        return -ENOENT;
    return -EIO;
}
