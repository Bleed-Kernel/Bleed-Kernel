#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <string.h>
#include <status.h>
#include <user/errno.h>
#include <user/user_copy.h>

int sys_rename(const char *user_oldpath, const char *user_newpath) {
    if (!user_oldpath || !user_newpath)
        return -EFAULT;

    char oldpath[256];
    char newpath[256];
    memset(oldpath, 0, sizeof(oldpath));
    memset(newpath, 0, sizeof(newpath));

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    if (copy_from_user(caller, oldpath, user_oldpath, sizeof(oldpath)) != 0)
        return -EFAULT;
    if (copy_from_user(caller, newpath, user_newpath, sizeof(newpath)) != 0)
        return -EFAULT;
    oldpath[sizeof(oldpath) - 1] = '\0';
    newpath[sizeof(newpath) - 1] = '\0';

    size_t oldlen = 0;
    while (oldlen < sizeof(oldpath) && oldpath[oldlen] != '\0')
        oldlen++;
    if (oldlen == sizeof(oldpath))
        return -E2BIG;

    size_t newlen = 0;
    while (newlen < sizeof(newpath) && newpath[newlen] != '\0')
        newlen++;
    if (newlen == sizeof(newpath))
        return -E2BIG;

    int r = vfs_rename(oldpath, newpath);
    if (r >= 0)
        return 0;

    if (r == -FILE_NOT_FOUND)
        return -ENOENT;
    if (r == -UNIMPLEMENTED)
        return -ENOSYS;
    if (r == -NAME_LIMITS)
        return -E2BIG;
    if (r == -OUT_OF_BOUNDS)
        return -EEXIST;
    if (r == -EXDEV)
        return -EXDEV;

    return -EIO;
}
