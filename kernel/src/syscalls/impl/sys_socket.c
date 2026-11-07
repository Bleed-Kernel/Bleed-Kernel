#include <net/socket.h>
#include <net/unix.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <fs/vfs.h>

long sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    long fd;
    if (domain == AF_UNIX)
        fd = protocol ? -EPROTONOSUPPORT : unix_socket_create((int)type);
    else
        fd = socket_create((int)domain, (int)type, (int)protocol);
    if (fd >= 0 && (type & SOCK_CLOEXEC))
        vfs_set_fd_flags((int)fd, FD_CLOEXEC);
    return fd;
}

long sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t user_fds) {
    if (domain != AF_UNIX)
        return -EOPNOTSUPP;
    if (protocol)
        return -EPROTONOSUPPORT;
    if (!user_fds)
        return -EFAULT;

    int fds[2];
    int err = unix_socketpair((int)type, fds);
    if (err)
        return err;
    if (type & SOCK_CLOEXEC) {
        vfs_set_fd_flags(fds[0], FD_CLOEXEC);
        vfs_set_fd_flags(fds[1], FD_CLOEXEC);
    }
    if (copy_to_user(get_current_task(), (void *)user_fds, fds, sizeof(fds)) != 0) {
        vfs_close(fds[0]);
        vfs_close(fds[1]);
        return -EFAULT;
    }
    return 0;
}
