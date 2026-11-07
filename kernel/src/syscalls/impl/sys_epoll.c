#include <fs/epoll.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

long sys_epoll_create(uint64_t size) {
    if ((int64_t)size <= 0)
        return -EINVAL;
    return epoll_create(0);
}

long sys_epoll_create1(uint64_t flags) {
    return epoll_create((int)flags);
}

long sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t user_event) {
    epoll_event_t ev = { 0, 0 };
    if (op != EPOLL_CTL_DEL &&
        (!user_event || copy_from_user(get_current_task(), &ev, (const void *)user_event, sizeof(ev)) != 0))
        return -EFAULT;
    return epoll_ctl((int)epfd, (int)op, (int)fd, &ev);
}

long sys_epoll_wait(uint64_t epfd, uint64_t user_events, uint64_t max, uint64_t timeout) {
    return epoll_wait((int)epfd, (void *)user_events, (int)max, (long)(int)timeout, NULL);
}

long sys_epoll_pwait(uint64_t epfd, uint64_t user_events, uint64_t max, uint64_t timeout,
                     uint64_t user_mask, uint64_t mask_size) {
    if (!user_mask)
        return sys_epoll_wait(epfd, user_events, max, timeout);
    if (mask_size != sizeof(sigset_t))
        return -EINVAL;

    sigset_t mask;
    if (copy_from_user(get_current_task(), &mask, (const void *)user_mask, sizeof(mask)) != 0)
        return -EFAULT;
    return epoll_wait((int)epfd, (void *)user_events, (int)max, (long)(int)timeout, &mask);
}
