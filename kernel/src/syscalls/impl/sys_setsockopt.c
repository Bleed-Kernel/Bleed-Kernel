#include <net/socket.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define SOCKOPT_MAX 64  // a timeval is the biggest

long sys_setsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t user_val, uint64_t len) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    uint8_t val[SOCKOPT_MAX] = {0};
    if (len > sizeof(val))
        len = sizeof(val);
    if (len && (!user_val || copy_from_user(get_current_task(), val, (const void *)user_val, len) != 0))
        return -EFAULT;
    return socket_setopt(s, (int)level, (int)name, val, len);
}

long sys_getsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t user_val, uint64_t user_len) {
    int       err;
    socket_t *s = socket_from_fd((int)fd, &err);
    if (!s)
        return err;

    task_t  *task = get_current_task();
    uint32_t room;
    if (!user_val || !user_len || copy_from_user(task, &room, (const void *)user_len, sizeof(room)) != 0)
        return -EFAULT;

    uint8_t val[SOCKOPT_MAX];
    size_t  len = room < sizeof(val) ? room : sizeof(val);
    err = socket_getopt(s, (int)level, (int)name, val, &len);
    if (err)
        return err;

    uint32_t out = (uint32_t)len;
    if (copy_to_user(task, (void *)user_val, val, len) != 0 || copy_to_user(task, (void *)user_len, &out, sizeof(out)) != 0)
        return -EFAULT;
    return 0;
}
