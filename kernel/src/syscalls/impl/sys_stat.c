#include <string.h>
#include <user/user_copy.h>
#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <syscalls/syscall.h>

static long stat_out(task_t *caller, user_stat_t *user_buf, const user_stat_t *st) {
    if (copy_to_user(caller, user_buf, st, sizeof(*st)) != 0)
        return -EFAULT;
    return 0;
}

long sys_stat(const char *user_path, user_stat_t *user_buf) {
    if (!user_path || !user_buf)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    char kpath[256];
    long plen = copy_user_path(caller, user_path, kpath, sizeof(kpath));
    if (plen == -2)
        return -ENAMETOOLONG;
    if (plen < 0)
        return -EFAULT;

    user_stat_t st;
    int r = vfs_stat(kpath, &st);
    if (r < 0)
        return r;
    return stat_out(caller, user_buf, &st);
}

long sys_lstat(const char *user_path, user_stat_t *user_buf) {
    return sys_fstatat(AT_FDCWD, user_path, user_buf, AT_SYMLINK_NOFOLLOW);
}

long sys_fstatat(int dirfd, const char *user_path, user_stat_t *user_buf, int flags) {
    if (!user_path || !user_buf)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    char kpath[256];
    long plen = copy_user_path(caller, user_path, kpath, sizeof(kpath));
    if (plen == -2)
        return -ENAMETOOLONG;
    if (plen < 0)
        return -EFAULT;

    user_stat_t st;
    int r = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat_at(dirfd, kpath, &st) : vfs_stat_at(dirfd, kpath, &st);
    if (r < 0)
        return r;
    return stat_out(caller, user_buf, &st);
}

/// @brief the same for something already open
long sys_fstat(int fd, user_stat_t *user_buf) {
    if (!user_buf)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    user_stat_t st;
    int r = vfs_fstat(fd, &st);
    if (r < 0)
        return r;
    return stat_out(caller, user_buf, &st);
}
