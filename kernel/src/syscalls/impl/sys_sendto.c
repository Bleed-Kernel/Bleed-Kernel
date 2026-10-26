#include <net/socket.h>
#include <syscalls/syscall.h>

long sys_sendto(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t flags, uint64_t user_addr, uint64_t addr_len) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    uint32_t ip   = 0;
    uint16_t port = 0;
    if (user_addr) {
        err = sockaddr_from_user((const void *)user_addr, addr_len, &ip, &port);
        if (err)
            return err;
    }
    return socket_sendto(s, (const void *)user_buf, len, (int)flags, user_addr != 0, ip, port);
}
