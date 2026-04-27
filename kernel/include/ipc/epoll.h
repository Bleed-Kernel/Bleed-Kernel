#pragma once

#include <stdint.h>
#include <mm/spinlock.h>

// event flag bits
#define EPOLLIN        (1u << 0)
#define EPOLLPRI       (1u << 1)
#define EPOLLOUT       (1u << 2)
#define EPOLLERR       (1u << 3)
#define EPOLLHUP       (1u << 4)
#define EPOLLRDNORM    (1u << 6)
#define EPOLLRDBAND    (1u << 7)
#define EPOLLWRNORM    (1u << 8)
#define EPOLLWRBAND    (1u << 9)
#define EPOLLMSG       (1u << 10)
#define EPOLLRDHUP     (1u << 13)
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP    (1u << 29)
#define EPOLLONESHOT   (1u << 30)
#define EPOLLET        (1u << 31)

// epoll control opcodes
#define EPOLL_CTL_ADD   1
#define EPOLL_CTL_DEL   2
#define EPOLL_CTL_MOD   3
#define EPOLL_CLOEXEC   (1 << 0)

struct task;
struct poll_table;
struct epoll_instance;

// userspace
typedef union epoll_data {
    void       *ptr;
    int         fd;
    uint32_t    u32;
    uint64_t    u64;
} epoll_data_t;

typedef struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed)) epoll_event_t;

// pt
typedef struct poll_wakeup {
    struct epoll_instance *instance;
    int                    fd;
    struct poll_wakeup    *next;
} poll_wakeup_t;

typedef struct poll_table {
    spinlock_t      lock;
    poll_wakeup_t  *head;
} poll_table_t;

// entry and instance
typedef struct epoll_entry {
    int                 fd;
    uint32_t            events;
    epoll_data_t        data;
    uint32_t            flags;
    int                 ready;
    struct epoll_entry *next;
    struct epoll_entry *ready_next;
} epoll_entry_t;

typedef struct epoll_instance {
    spinlock_t      lock;
    epoll_entry_t  *entries;
    int             entry_count;
    epoll_entry_t  *ready_head;
    epoll_entry_t  *ready_tail;
    int             ready_count;
    struct task    *waiter_task;
    int             refcount;
} epoll_instance_t;

void poll_table_init(poll_table_t *pt);
void poll_notify(poll_table_t *pt, uint32_t events);

epoll_instance_t *epoll_instance_create(void);
void epoll_instance_put(epoll_instance_t *ep);
void epoll_instance_destroy(epoll_instance_t *ep);

int epoll_ctl_add(epoll_instance_t *ep, int fd, epoll_event_t *ev,
                  poll_table_t *pt);
int epoll_ctl_mod(epoll_instance_t *ep, int fd, epoll_event_t *ev);
int epoll_ctl_del(epoll_instance_t *ep, int fd, poll_table_t *pt);
int epoll_wait_events(epoll_instance_t *ep, struct task *task,
                      epoll_event_t *out, int maxevents, int timeout_ms);

long sys_epoll_create1(int flags);
long sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *user_ev);
long sys_epoll_wait(int epfd, epoll_event_t *user_events,
                    int maxevents, int timeout_ms);

void ipc_poll_notify_readable(struct task *task);
void ipc_poll_check_writable(struct task *task);