#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <devices/devices.h>
#include <sched/scheduler.h>
#include <string.h>
#include <status.h>
#include <user/errno.h>
#include <user/user_copy.h>

int sys_umount2(const char *user_target, int flags) {
    (void)flags;

    if (!user_target)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    char ktarget[256];
    memset(ktarget, 0, sizeof(ktarget));
    long tlen = copy_user_path(caller, user_target, ktarget, sizeof(ktarget));
    if (tlen == -2)
        return -ENAMETOOLONG;
    if (tlen < 0)
        return -EFAULT;

    int r = vfs_umount(ktarget);
    if (r >= 0)
        return 0;

    if (r == -FILE_NOT_FOUND)  return -ENOENT;
    if (r == -UNIMPLEMENTED)   return -ENOSYS;

    return -EINVAL;
}