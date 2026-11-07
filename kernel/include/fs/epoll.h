#pragma once

#include <stdint.h>
#include <sched/signal.h>

typedef struct file file_t;

#define EPOLLRDHUP      0x2000
#define EPOLLONESHOT    (1u << 30)
#define EPOLLET         (1u << 31)

#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3
#define EPOLL_CLOEXEC   0x80000

typedef struct __attribute__((packed)) {
    uint32_t events;
    uint64_t data;
} epoll_event_t;

int  epoll_create(int flags);
int  epoll_ctl(int epfd, int op, int fd, const epoll_event_t *ev);
long epoll_wait(int epfd, void *user_events, int max, long timeout_ms, const sigset_t *mask);

void epoll_file_release(file_t *f);
