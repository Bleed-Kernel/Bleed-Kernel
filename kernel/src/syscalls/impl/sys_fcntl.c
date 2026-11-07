#include <fs/vfs.h>
#include <syscalls/syscall.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <fs/memfd.h>
#include <sched/scheduler.h>

#define F_DUPFD             0
#define F_GETFD             1
#define F_SETFD             2
#define F_GETFL             3
#define F_DUPFD_CLOEXEC     1030
#define F_ADD_SEALS         1033
#define F_GET_SEALS         1034

// the fd side and the open flags. F_SETFL stays in blibc on top of the nonblock ioctls
long sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    switch (cmd) {
        case F_DUPFD:
            return vfs_dup_from((int)fd, (int)arg);
        case F_DUPFD_CLOEXEC: {
            int nfd = vfs_dup_from((int)fd, (int)arg);
            if (nfd >= 0)
                vfs_set_fd_flags(nfd, FD_CLOEXEC);
            return nfd;
        }
        case F_GETFD:
            return vfs_get_fd_flags((int)fd);
        case F_SETFD:
            return vfs_set_fd_flags((int)fd, (int)arg);
        case F_GETFL: {
            // the access mode and status flags it was opened with, creation flags arent kept
            file_t *f = vfs_file_get((int)fd);
            if (!f)
                return -EBADF;
            long r = f->flags & (O_MODE | O_APPEND | O_NONBLOCK);
            vfs_file_put(f);
            return r;
        }
        case F_ADD_SEALS:
        case F_GET_SEALS: {
            file_t *f = vfs_file_get((int)fd);
            if (!f)
                return -EBADF;
            long r = -EINVAL;
            if (memfd_is(f->inode))
                r = cmd == F_ADD_SEALS ? memfd_add_seals(f->inode, (uint32_t)arg) : memfd_get_seals(f->inode);
            vfs_file_put(f);
            return r;
        }
        default:
            return -EINVAL;
    }
}

long sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    if (oldfd == newfd)
        return -EINVAL;
    if (flags & ~(uint64_t)O_CLOEXEC)
        return -EINVAL;
    int r = vfs_dup2((int)oldfd, (int)newfd);
    if (r >= 0 && (flags & O_CLOEXEC))
        vfs_set_fd_flags(r, FD_CLOEXEC);
    return r;
}

long sys_memfd_create(uint64_t user_name, uint64_t flags) {
    char name[64];
    if (!user_name)
        return -EFAULT;
    long n = copy_user_path(get_current_task(), (const char *)user_name, name, sizeof(name));
    if (n < 0)
        return n == -2 ? -EINVAL : -EFAULT;
    return memfd_create(name, (unsigned int)flags);
}

long sys_ftruncate(uint64_t fd, uint64_t length) {
    if ((int64_t)length < 0)
        return -EINVAL;
    file_t *f = vfs_file_get((int)fd);
    if (!f)
        return -EBADF;

    long r;
    if (f->type != FD_TYPE_FS || !f->inode || f->inode->type != INODE_FILE)
        r = -EINVAL;
    else if ((f->flags & O_MODE) == O_RDONLY)
        r = -EINVAL;
    else
        r = inode_truncate(f->inode, (size_t)length);
    vfs_file_put(f);
    return r;
}
