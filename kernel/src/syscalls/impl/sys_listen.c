#include <net/socket.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

long sys_listen(uint64_t fd, uint64_t backlog) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    return socket_listen(s, (int)backlog);
}

long sys_accept4(uint64_t fd, uint64_t user_addr, uint64_t user_len, uint64_t flags) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;

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
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    return socket_shutdown(s, (int)how);
}
