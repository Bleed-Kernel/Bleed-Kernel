#include <mm/kalloc.h>
#include <string.h>
#include <user/user_copy.h>
#include <fs/vfs.h>

int sys_stat(int fd, user_file_t *user_buf) {
    if (!user_buf) return -1;

    file_t *kfile = NULL;
    user_file_t userfile;

    if (current_fd_table->fds[fd] != NULL) {
        kfile = current_fd_table->fds[fd];
        memset(&userfile, 0, sizeof(user_file_t));
        userfile.filesize = vfs_filesize(kfile->inode);
        userfile.permissions = kfile->flags;
        strncpy(userfile.fname, kfile->inode->internal_data, sizeof(userfile.fname) - 1);
        userfile.fname[sizeof(userfile.fname) - 1] = '\0';
        

        if (copy_to_user(user_buf, &userfile, sizeof(user_file_t)) != 0)
            return -1;

        return 0;
    }

    return -1;
}
