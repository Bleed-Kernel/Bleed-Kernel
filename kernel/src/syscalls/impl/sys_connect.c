#include <net/socket.h>
#include <syscalls/syscall.h>
#include <user/user_copy.h>
#include <sched/scheduler.h>
#include <user/errno.h>

#define AF_UNSPEC 0

long sys_connect(uint64_t fd, uint64_t user_addr, uint64_t len) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    // AF_UNSPEC undoes a connect 
    uint16_t family = 0;
    if (len >= sizeof(family) && user_addr &&
        copy_from_user(get_current_task(), &family, (const void *)user_addr, sizeof(family)) != 0)
        return -EFAULT;
    if (family == AF_UNSPEC)
        return socket_disconnect(s);

    uint32_t ip;
    uint16_t port;
    err = sockaddr_from_user((const void *)user_addr, len, &ip, &port);
    if (err)
        return err;
    return socket_connect(s, ip, port);
}
