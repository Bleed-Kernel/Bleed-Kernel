#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <status.h>
#include <string.h>

long sys_readdir(int fd, size_t index, dirent_t *user_ent) {
    if (!user_ent)
        return status_print_error(FILE_NOT_FOUND);

    if (!current_fd_table || fd < 0 || fd >= MAX_FDS)
        return status_print_error(FILE_NOT_FOUND);

    file_t *f = current_fd_table->fds[fd];
    if (!f)
        return status_print_error(FILE_NOT_FOUND);

    INode_t *dir = f->inode;
    if (!dir || dir->type != INODE_DIRECTORY)
        return status_print_error(FILE_NOT_FOUND);

    SMAP_ALLOW{
        INode_t *child = NULL;
        int r = vfs_readdir(dir, index, &child);
        if (r < 0)
            return r;

        if (!child || !child->internal_data)
            return status_print_error(FILE_NOT_FOUND);


        memset(user_ent, 0, sizeof(*user_ent));
        strncpy(user_ent->name, child->internal_data, sizeof(user_ent->name) - 1);
        user_ent->type = child->type;
    }

    return 0;
}
