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

    if (fd >= MAX_FDS || !current_fd_table)
        return -EBADF;

    kfile = current_fd_table->fds[fd];
    if (!kfile)
        return -EBADF;
    if (!kfile->inode)
        return -EBADF;

    memset(&userfile, 0, sizeof(user_file_t));
    userfile.filesize = vfs_filesize(kfile->inode);
    userfile.permissions = kfile->flags;
    strncpy(userfile.fname, kfile->inode->internal_data, sizeof(userfile.fname) - 1);
    userfile.fname[sizeof(userfile.fname) - 1] = '\0';

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    if (copy_to_user(caller, user_buf, &userfile, sizeof(user_file_t)) != 0)
        return -EFAULT;

    return 0;
}
