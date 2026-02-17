#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <string.h>
#include <user/user_copy.h>
#include <user/errno.h>

long sys_readdir(int fd, size_t index, dirent_t *user_ent) {
    if (!user_ent)
        return -EFAULT;

    if (!current_fd_table || fd < 0 || fd >= MAX_FDS)
        return -EBADF;

    file_t *f = current_fd_table->fds[fd];
    if (!f)
        return -EBADF;

    INode_t *dir = f->inode;
    if (!dir || dir->type != INODE_DIRECTORY)
        return -ENOTDIR;

    INode_t *child = NULL;
    int r = vfs_readdir(dir, index, &child);
    if (r < 0)
        return -ENOENT;

    if (!child || !child->internal_data)
        return -ENOENT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    dirent_t kent;
    memset(&kent, 0, sizeof(kent));
    strncpy(kent.name, child->internal_data, sizeof(kent.name) - 1);
    kent.type = child->type;

    if (copy_to_user(caller, user_ent, &kent, sizeof(kent)) != 0)
        return -EFAULT;

    return 0;
}
