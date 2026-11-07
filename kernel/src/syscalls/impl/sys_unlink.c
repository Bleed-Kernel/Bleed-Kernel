#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_copy.h>

long sys_unlink(const char *user_path) {
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

#define AT_REMOVEDIR 0x200

// its the flag that decides if theres an error
long sys_unlinkat(int dirfd, const char *user_path, int flags) {
    if (!user_path)
        return -EFAULT;
    if (flags & ~AT_REMOVEDIR)
        return -EINVAL;

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));
    long plen = copy_user_path(get_current_task(), user_path, kpath, sizeof(kpath));
    if (plen == -2)
        return -E2BIG;
    if (plen < 0)
        return -EFAULT;

    user_stat_t st;
    int r = vfs_stat_at(dirfd, kpath, &st);
    if (r < 0)
        return r;
    bool dir = (st.mode & STAT_IFMT) == STAT_IFDIR;
    if ((flags & AT_REMOVEDIR) && !dir)
        return -ENOTDIR;
    if (!(flags & AT_REMOVEDIR) && dir)
        return -EISDIR;

    r = vfs_unlink_at(dirfd, kpath);
    return r < 0 ? r : 0;
}
