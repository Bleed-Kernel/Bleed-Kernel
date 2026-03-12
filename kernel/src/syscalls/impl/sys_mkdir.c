#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <string.h>
#include <status.h>
#include <user/errno.h>
#include <user/user_copy.h>

int sys_mkdir(const char *user_path, int mode) {
    (void)mode;

    if (!user_path)
        return -EFAULT;

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    if (copy_from_user(caller, kpath, user_path, sizeof(kpath)) != 0)
        return -EFAULT;
    kpath[sizeof(kpath) - 1] = '\0';

    size_t plen = 0;
    while (plen < sizeof(kpath) && kpath[plen] != '\0')
        plen++;
    if (plen == sizeof(kpath))
        return -E2BIG;

    int r = vfs_mkdir(kpath);
    if (r >= 0)
        return 0;

    if (r == -FILE_NOT_FOUND)
        return -ENOENT;
    if (r == -UNIMPLEMENTED)
        return -ENOSYS;
    if (r == -NAME_LIMITS)
        return -E2BIG;
    if (r == -OUT_OF_BOUNDS)
        return -EINVAL;
    if (r == -EXDEV)
        return -EXDEV;
    if (r == -EEXIST)
        return -EEXIST;

    return -EIO;
}
