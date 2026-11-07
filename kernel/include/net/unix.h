#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct file file_t;
typedef struct unix_sock unix_sock_t;

#define AF_UNIX             1
#define SOCK_SEQPACKET      5

#define SCM_RIGHTS          1
#define SO_PASSCRED         16
#define SO_PEERCRED         17

#define MSG_TRUNC           0x20
#define MSG_CTRUNC          0x8
#define MSG_NOSIGNAL        0x4000
#define MSG_CMSG_CLOEXEC    0x40000000

#define UNIX_PATH_MAX       108
#define UNIX_FDS_MAX        64          // per message, linux allows 253
#define UNIX_RX_MAX         (256 * 1024) // queued bytes before senders wait
#define UNIX_DGRAM_MAX      (208 * 1024)

// the linux layouts
typedef struct {
    uint64_t base;
    uint64_t len;
} user_iovec_t;

typedef struct {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t  flags;
    uint32_t pad1;
} user_msghdr_t;

typedef struct {
    uint64_t len;
    int32_t  level;
    int32_t  type;
} user_cmsghdr_t;

int  unix_socket_create(int type);
int  unix_socketpair(int type, int fds[2]);

// the unix socket behind fd, NULL if its something else
unix_sock_t *unix_from_fd(int fd);

int  unix_bind(unix_sock_t *u, const void *user_addr, uint64_t len);
int  unix_connect(unix_sock_t *u, const void *user_addr, uint64_t len);
int  unix_listen(unix_sock_t *u, int backlog);
int  unix_accept(unix_sock_t *u, int flags, void *user_addr, uint32_t *user_len);
int  unix_shutdown(unix_sock_t *u, int how);
int  unix_name(unix_sock_t *u, bool peer, void *user_addr, uint32_t *user_len);
int  unix_setopt(unix_sock_t *u, int level, int name, const void *val, size_t len);
int  unix_getopt(unix_sock_t *u, int level, int name, void *val, size_t *len);

// msg is a kernel copy of the users msghdr, the pointers in it are still user ones
long unix_sendmsg(unix_sock_t *u, const user_msghdr_t *msg, int flags);
long unix_recvmsg(unix_sock_t *u, user_msghdr_t *msg, int flags);

// the plain calls, one user buffer and no fds
long unix_sendto(unix_sock_t *u, const void *user_buf, size_t len, int flags, const void *user_addr, uint64_t addr_len);
long unix_recvfrom(unix_sock_t *u, void *user_buf, size_t len, int flags, void *user_addr, uint32_t *user_len);

void unix_file_release(file_t *f);
