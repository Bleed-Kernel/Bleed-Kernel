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

    long oldlen = copy_user_path(caller, user_oldpath, oldpath, sizeof(oldpath));
    if (oldlen == -2)
        return -E2BIG;
    if (oldlen < 0)
        return -EFAULT;

    long newlen = copy_user_path(caller, user_newpath, newpath, sizeof(newpath));
    if (newlen == -2)
        return -E2BIG;
    if (newlen < 0)
        return -EFAULT;

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
