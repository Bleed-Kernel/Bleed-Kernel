#include <net/socket.h>
#include <net/unix.h>
#include <syscalls/syscall.h>
#include <user/errno.h>
#include <fs/vfs.h>

long sys_listen(uint64_t fd, uint64_t backlog) {
    unix_sock_t *u = unix_from_fd((int)fd);
    if (u)
        return unix_listen(u, (int)backlog);

    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    return socket_listen(s, (int)backlog);
}

static long accept_flags(uint64_t fd, uint64_t user_addr, uint64_t user_len, uint64_t flags);

long sys_accept4(uint64_t fd, uint64_t user_addr, uint64_t user_len, uint64_t flags) {
    long new_fd = accept_flags(fd, user_addr, user_len, flags);
    if (new_fd >= 0 && (flags & SOCK_CLOEXEC))
        vfs_set_fd_flags((int)new_fd, FD_CLOEXEC);
    return new_fd;
}

static long accept_flags(uint64_t fd, uint64_t user_addr, uint64_t user_len, uint64_t flags) {
    if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    unix_sock_t *u = unix_from_fd((int)fd);
    if (u)
        return unix_accept(u, (int)flags, (void *)user_addr, (uint32_t *)user_len);

    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    uint32_t ip;
    uint16_t port;
    int      new_fd = socket_accept(s, (int)flags, &ip, &port);
    if (new_fd < 0)
        return new_fd;

    // the connection is ours by now, a bad pointer doesnt undo it
    err = sockaddr_to_user((void *)user_addr, (uint32_t *)user_len, ip, port);
    return err ? err : new_fd;
}

long sys_accept(uint64_t fd, uint64_t user_addr, uint64_t user_len) {
    return sys_accept4(fd, user_addr, user_len, 0);
}

long sys_sock_shutdown(uint64_t fd, uint64_t how) {
    unix_sock_t *u = unix_from_fd((int)fd);
    if (u)
        return unix_shutdown(u, (int)how);

    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    return socket_shutdown(s, (int)how);
}
