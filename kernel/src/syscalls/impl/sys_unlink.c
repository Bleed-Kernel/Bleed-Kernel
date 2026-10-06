#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_copy.h>

int sys_unlink(const char *user_path) {
    if (!user_path)
        return -EFAULT;

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    long plen = copy_user_path(caller, user_path, kpath, sizeof(kpath));
    if (plen == -2)
        return -E2BIG;
    if (plen < 0)
        return -EFAULT;

    int r = vfs_unlink(kpath);
    return r < 0 ? r : 0;
}
