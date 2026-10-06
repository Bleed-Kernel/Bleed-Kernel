#include <sched/scheduler.h>
#include <fs/vfs.h>
#include <string.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <mm/kalloc.h>

long sys_getcwd(char *buf, long size) {
    if (!buf || size <= 0)
        return -EFAULT;

    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    if (!task->current_directory)
        return -ENOENT;

    INode_t *inode = task->current_directory;

    // fast root
    if (inode == vfs_get_root()) {
        if (size < 2)
            return -ERANGE;
        if (copy_to_user(task, buf, "/", 2) != 0)
            return -EFAULT;
        return 0;
    }

    size_t total_len = 0;
    INode_t *cur = inode;
    while (cur && cur != vfs_get_root()) {
        size_t nlen = strlen(cur->name);
        if (nlen == 0) {
            return -EIO;
        }
        total_len += 1 + nlen;  // '/{name}'
        cur = cur->parent;
    }

    // +1 for null terminator, path itself starts with '/' already accounted for
    if (total_len + 1 > (size_t)size || total_len + 1 > PATH_MAX)
        return -ERANGE;

    // sized to the path, a PATH_MAX array is half of an 8K kernel stack
    char *kpath = kmalloc(total_len + 1);
    if (!kpath)
        return -ENOMEM;

    kpath[total_len] = '\0';
    cur = inode;
    size_t pos = total_len;
    while (cur && cur != vfs_get_root()) {
        size_t nlen = strlen(cur->name);
        pos -= nlen;
        memcpy(kpath + pos, cur->name, nlen);
        pos--;
        kpath[pos] = '/';
        cur = cur->parent;
    }

    int rc = copy_to_user(task, buf, kpath, total_len + 1);
    kfree(kpath);

    return rc != 0 ? -EFAULT : 0;
}
