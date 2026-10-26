#include <net/socket.h>
#include <syscalls/syscall.h>

long sys_recvfrom(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t flags, uint64_t user_addr, uint64_t user_addr_len) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    uint32_t ip   = 0;
    uint16_t port = 0;
    long     got  = socket_recvfrom(s, (void *)user_buf, len, (int)flags, &ip, &port);
    if (got < 0)
        return got;

    err = sockaddr_to_user((void *)user_addr, (uint32_t *)user_addr_len, ip, port);
    return err ? err : got;
}
