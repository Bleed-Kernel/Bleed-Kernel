#include <mm/kalloc.h>
#include <string.h>
#include <user/user_copy.h>
#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/errno.h>

int sys_stat(int fd, user_file_t *user_buf) {
    if (!user_buf)
        return -EFAULT;

    file_t *kfile = NULL;
    user_file_t userfile;
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    if (fd < 0 || fd >= MAX_FDS || !caller->fd_table)
        return -EBADF;

    kfile = caller->fd_table->fds[fd];
    if (!kfile)
        return -EBADF;
    if (!kfile->inode)
        return -EBADF;

    memset(&userfile, 0, sizeof(user_file_t));
    userfile.filesize = vfs_filesize(kfile->inode);
    userfile.permissions = kfile->flags;
    if (kfile->inode->type == INODE_DEVICE) {
        strncpy(userfile.fname, "device", sizeof(userfile.fname) - 1);
    } else if (kfile->inode->internal_data) {
        strncpy(userfile.fname, kfile->inode->internal_data, sizeof(userfile.fname) - 1);
    }
    userfile.fname[sizeof(userfile.fname) - 1] = '\0';

    if (copy_to_user(caller, user_buf, &userfile, sizeof(user_file_t)) != 0)
        return -EFAULT;

    return 0;
}
