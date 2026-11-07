#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <syscalls/syscall.h>

long sys_symlinkat(const char *user_target, int dirfd, const char *user_path) {
    if (!user_target || !user_path)
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

    char *target = kmalloc(VFS_LINK_MAX);
    if (!target)
        return -ENOMEM;
    long tlen = copy_user_path(caller, user_target, target, VFS_LINK_MAX);
    long r;
    if (tlen == -2)
        r = -ENAMETOOLONG;
    else if (tlen < 0)
        r = -EFAULT;
    else
        r = vfs_symlink_at(target, dirfd, kpath);
    kfree(target);
    return r;
}

// the target comes back without a nul and the return is its length just like linux
long sys_readlinkat(int dirfd, const char *user_path, char *user_buf, uint64_t size) {
    if (!user_path || !user_buf)
        return -EFAULT;
    if (!size)
        return -EINVAL;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    char kpath[256];
    long plen = copy_user_path(caller, user_path, kpath, sizeof(kpath));
    if (plen == -2)
        return -ENAMETOOLONG;
    if (plen < 0)
        return -EFAULT;

    char *target = kmalloc(VFS_LINK_MAX);
    if (!target)
        return -ENOMEM;
    long n = vfs_readlink_at(dirfd, kpath, target, VFS_LINK_MAX);
    if (n > 0) {
        if ((uint64_t)n > size)
            n = (long)size;
        if (copy_to_user(caller, user_buf, target, (size_t)n) != 0)
            n = -EFAULT;
    }
    kfree(target);
    return n;
}
