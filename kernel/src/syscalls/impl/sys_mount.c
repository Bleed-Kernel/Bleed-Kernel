#include <fs/vfs.h>
#include <fs/vfs_mount.h>
#include <devices/devices.h>
#include <sched/scheduler.h>
#include <string.h>
#include <status.h>
#include <user/errno.h>
#include <user/user_copy.h>

int sys_mount(const char *user_source,
              const char *user_target,
              const char *user_fstype,
              unsigned long mountflags,
              const void  *data) {
    (void)user_fstype;
    (void)mountflags;
    (void)data;

    if (!user_source || !user_target)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    // src path copy
    char ksource[256];
    memset(ksource, 0, sizeof(ksource));
    if (copy_from_user(caller, ksource, user_source, sizeof(ksource)) != 0)
        return -EFAULT;
    ksource[sizeof(ksource) - 1] = '\0';

    size_t slen = strnlen(ksource, sizeof(ksource));
    if (slen == sizeof(ksource))
        return -ENAMETOOLONG;

    // target
    char ktarget[256];
    memset(ktarget, 0, sizeof(ktarget));
    if (copy_from_user(caller, ktarget, user_target, sizeof(ktarget)) != 0)
        return -EFAULT;
    ktarget[sizeof(ktarget) - 1] = '\0';

    size_t tlen = strnlen(ktarget, sizeof(ktarget));
    if (tlen == sizeof(ktarget))
        return -ENAMETOOLONG;

    /* Only block devices under /dev/ are valid sources */
    if (strncmp(ksource, "/dev/", 5) != 0)
        return -ENOTBLK;

    const char *dev_name = ksource + 5;
    if (*dev_name == '\0')
        return -ENOTBLK;

    INode_t *dev_inode = device_get_by_name(dev_name);
    if (!dev_inode)
        return -ENODEV;

    int r = vfs_mount(ktarget, dev_inode);
    if (r >= 0)
        return 0;

    if (r == -FILE_NOT_FOUND)  return -ENOENT;
    if (r == -UNIMPLEMENTED)   return -ENOSYS;
    if (r == -NAME_LIMITS)     return -ENAMETOOLONG;
    if (r == -OUT_OF_MEMORY)   return -ENOMEM;

    return -EIO;
}