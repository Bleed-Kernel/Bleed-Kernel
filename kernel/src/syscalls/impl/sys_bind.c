#include <net/socket.h>
#include <syscalls/syscall.h>

long sys_bind(uint64_t fd, uint64_t user_addr, uint64_t len) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    uint32_t ip;
    uint16_t port;
    err = sockaddr_from_user((const void *)user_addr, len, &ip, &port);
    if (err)
        return err;
    return socket_bind(s, ip, port);
}
