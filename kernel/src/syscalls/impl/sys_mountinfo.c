#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <fs/vfs_mount.h>
#include <user/user_copy.h>
#include <user/user_mount.h>
#include <user/errno.h>

long sys_mountinfo(uint64_t index, uint64_t user_info_ptr) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (!user_info_ptr)
        return -EFAULT;
    if (index > VFS_MAX_MOUNTS)
        return -ENOENT;

    user_mount_info_t info;
    int r = vfs_mount_info((int)index, &info);
    if (r < 0)
        return r;

    if (copy_to_user(caller, (void *)user_info_ptr, &info, sizeof(info)) != 0)
        return -EFAULT;
    return 0;
}
