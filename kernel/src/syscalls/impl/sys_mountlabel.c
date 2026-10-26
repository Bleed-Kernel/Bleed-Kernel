#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <fs/vfs_mount.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <string.h>

#define LABEL_MAX 64

// separate so mountinfos struct doesnt change size
long sys_mountlabel(uint64_t index, uint64_t user_buf, uint64_t size) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (!user_buf || !size)
        return -EFAULT;
    if (index > VFS_MAX_MOUNTS)
        return -ENOENT;

    char label[LABEL_MAX];
    int  r = vfs_mount_label((int)index, label, sizeof(label));
    if (r < 0)
        return r;

    size_t n = strlen(label) + 1;
    if (n > size)
        n = size;
    label[n - 1] = '\0';
    if (copy_to_user(caller, (void *)user_buf, label, n) != 0)
        return -EFAULT;
    return 0;
}
