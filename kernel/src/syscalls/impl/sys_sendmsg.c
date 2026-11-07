#include <net/socket.h>
#include <net/unix.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>
#include <stddef.h>
#include <mm/kalloc.h>

#define INET_MSG_MAX    (64 * 1024)

static int iov_from_user(const user_msghdr_t *msg, user_iovec_t **out, size_t *total) {
    *out   = NULL;
    *total = 0;
    if (msg->iovlen > 1024)
        return -EMSGSIZE;
    if (!msg->iovlen)
        return 0;

    user_iovec_t *v = kmalloc(msg->iovlen * sizeof(*v));
    if (!v)
        return -ENOMEM;
    if (copy_from_user(get_current_task(), v, (const void *)msg->iov, msg->iovlen * sizeof(*v)) != 0) {
        kfree(v);
        return -EFAULT;
    }
    for (uint64_t i = 0; i < msg->iovlen; i++)
        *total += v[i].len;
    *out = v;
    return 0;
}

// inet sockets take one buffer, so the pieces are joined up (a datagram has to stay whole)
static long inet_sendmsg(socket_t *s, const user_msghdr_t *msg, int flags) {
    task_t  *task = get_current_task();
    uint32_t ip   = 0;
    uint16_t port = 0;
    if (msg->name && msg->namelen) {
        int err = sockaddr_from_user((const void *)msg->name, msg->namelen, &ip, &port);
        if (err)
            return err;
    }

    user_iovec_t *v;
    size_t        total;
    int err = iov_from_user(msg, &v, &total);
    if (err)
        return err;
    if (total > INET_MSG_MAX)
        total = INET_MSG_MAX;

    uint8_t *flat = kmalloc(total ? total : 1);
    if (!flat) {
        kfree(v);
        return -ENOMEM;
    }
    size_t at = 0;
    for (uint64_t i = 0; i < msg->iovlen && at < total; i++) {
        size_t n = v[i].len < total - at ? v[i].len : total - at;
        if (n && copy_from_user(task, flat + at, (const void *)v[i].base, n) != 0) {
            kfree(flat);
            kfree(v);
            return -EFAULT;
        }
        at += n;
    }
    kfree(v);

    long r = socket_send_kernel(s, flat, total, flags, msg->name != 0, ip, port);
    kfree(flat);
    return r;
}

static long inet_recvmsg(socket_t *s, user_msghdr_t *msg, uint64_t user_msg, int flags) {
    task_t       *task = get_current_task();
    user_iovec_t *v;
    size_t        total;
    int err = iov_from_user(msg, &v, &total);
    if (err)
        return err;
    if (total > INET_MSG_MAX)
        total = INET_MSG_MAX;

    uint8_t *flat = kmalloc(total ? total : 1);
    if (!flat) {
        kfree(v);
        return -ENOMEM;
    }

    uint32_t ip   = 0;
    uint16_t port = 0;
    long     got  = socket_recv_kernel(s, flat, total, flags, &ip, &port);
    size_t   at   = 0;
    for (uint64_t i = 0; got > 0 && i < msg->iovlen && at < (size_t)got; i++) {
        size_t n = v[i].len < (size_t)got - at ? v[i].len : (size_t)got - at;
        if (n && copy_to_user(task, (void *)v[i].base, flat + at, n) != 0) {
            got = -EFAULT;
            break;
        }
        at += n;
    }
    kfree(flat);
    kfree(v);
    if (got < 0)
        return got;

    msg->controllen = 0;
    msg->flags      = 0;
    if (msg->name) {
        uint32_t *user_len = (uint32_t *)(user_msg + offsetof(user_msghdr_t, namelen));
        err = sockaddr_to_user((void *)msg->name, user_len, ip, port);
        if (err)
            return err;
        if (copy_from_user(task, &msg->namelen, user_len, sizeof(msg->namelen)) != 0)
            return -EFAULT;
    }
    return got;
}

long sys_sendmsg(uint64_t fd, uint64_t user_msg, uint64_t flags) {
    user_msghdr_t msg;
    if (!user_msg || copy_from_user(get_current_task(), &msg, (const void *)user_msg, sizeof(msg)) != 0)
        return -EFAULT;

    unix_sock_t *u = unix_from_fd((int)fd);
    if (u)
        return unix_sendmsg(u, &msg, (int)flags);

    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;
    return inet_sendmsg(s, &msg, (int)flags);
}

long sys_recvmsg(uint64_t fd, uint64_t user_msg, uint64_t flags) {
    task_t       *task = get_current_task();
    user_msghdr_t msg;
    if (!user_msg || copy_from_user(task, &msg, (const void *)user_msg, sizeof(msg)) != 0)
        return -EFAULT;

    long r;
    unix_sock_t *u = unix_from_fd((int)fd);
    if (u) {
        r = unix_recvmsg(u, &msg, (int)flags);
    } else {
        int       err;
        socket_t *s = socket_from_fd((int)fd, &err);
        if (!s)
            return err;
        r = inet_recvmsg(s, &msg, user_msg, (int)flags);
    }
    if (r < 0)
        return r;

    // namelen, controllen and flags are what the caller reads back
    if (copy_to_user(task, (void *)(user_msg + offsetof(user_msghdr_t, namelen)), &msg.namelen, sizeof(msg.namelen)) != 0 ||
        copy_to_user(task, (void *)(user_msg + offsetof(user_msghdr_t, controllen)), &msg.controllen, sizeof(msg.controllen)) != 0 ||
        copy_to_user(task, (void *)(user_msg + offsetof(user_msghdr_t, flags)), &msg.flags, sizeof(msg.flags)) != 0)
        return -EFAULT;
    return r;
}
