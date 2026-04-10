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
    if (copy_from_user(caller, ktarget, user_target, sizeof(ktarget)) != 0)
        return -EFAULT;
    ktarget[sizeof(ktarget) - 1] = '\0';
 
    size_t tlen = 0;
    while (tlen < sizeof(ktarget) && ktarget[tlen] != '\0') tlen++;
    if (tlen == sizeof(ktarget))
        return -E2BIG;
 
    int r = vfs_umount(ktarget);
 
    if (r >= 0)
        return 0;
 
    if (r == -FILE_NOT_FOUND)  return -ENOENT;
    if (r == -UNIMPLEMENTED)   return -ENOSYS;
 
    return -EINVAL;
}