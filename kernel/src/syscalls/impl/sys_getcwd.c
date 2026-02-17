#include <sched/scheduler.h>
#include <fs/vfs.h>
#include <string.h>
#include <user/user_copy.h>
#include <user/errno.h>

long sys_getcwd(char *buf, long size) {
    if (!buf || size <= 0)
        return -EFAULT;

    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    if (!task->current_directory)
        return -ENOENT;

    INode_t *inode = task->current_directory;
    char kpath[PATH_MAX];

    if (inode == vfs_get_root()) {
        if (size < 2)
            return -ERANGE;
        kpath[0] = '/';
        kpath[1] = '\0';
        if (copy_to_user(task, buf, kpath, 2) != 0)
            return -EFAULT;
        return 0;
    }

    size_t total_len = 0;
    INode_t *cur = inode;
    while (cur && cur != vfs_get_root()) {
        if (!cur->internal_data)
            return -EIO;
        total_len += strlen(cur->internal_data) + 1;
        cur = cur->parent;
    }

    if (total_len + 1 > (size_t)size || total_len + 1 > sizeof(kpath))
        return -ERANGE;

    kpath[total_len] = '\0';
    cur = inode;
    size_t pos = total_len;
    while (cur && cur != vfs_get_root()) {
        const char *name = cur->internal_data;
        size_t len = strlen(name);
        pos -= len;
        memcpy(kpath + pos, name, len);
        pos--;
        kpath[pos] = '/';
        cur = cur->parent;
    }

    if (copy_to_user(task, buf, kpath, total_len + 1) != 0)
        return -EFAULT;

    return 0;
}
