#include <net/socket.h>
#include <syscalls/syscall.h>
#include <user/errno.h>

static long sock_name(uint64_t fd, uint64_t user_addr, uint64_t user_len, bool peer) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    if (!user_addr || !user_len)
        return -EFAULT;

    uint32_t ip;
    uint16_t port;
    socket_name(s, peer, &ip, &port);
    if (peer && !port)
        return -ENOTCONN;
    return sockaddr_to_user((void *)user_addr, (uint32_t *)user_len, ip, port);
}

long sys_getsockname(uint64_t fd, uint64_t user_addr, uint64_t user_len) {
    return sock_name(fd, user_addr, user_len, false);
}

long sys_getpeername(uint64_t fd, uint64_t user_addr, uint64_t user_len) {
    return sock_name(fd, user_addr, user_len, true);
}
