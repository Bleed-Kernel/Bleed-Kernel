#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <net/net.h>

typedef struct file file_t;
typedef struct socket socket_t;

// the linux numbers, blibc uses the same
#define AF_INET             2
#define SOCK_STREAM         1
#define SOCK_DGRAM          2
#define SOCK_TYPE_MASK      0xF
#define SOCK_NONBLOCK       0x800
#define SOCK_CLOEXEC        0x80000

#define IPPROTO_IP          0
#define IPPROTO_TCP         6
#define IPPROTO_UDP         17

#define SOL_SOCKET          1
#define SO_REUSEADDR        2
#define SO_TYPE             3
#define SO_ERROR            4
#define SO_BROADCAST        6
#define SO_SNDBUF           7
#define SO_RCVBUF           8
#define SO_KEEPALIVE        9
#define SO_RCVTIMEO         20
#define SO_SNDTIMEO         21

#define IP_TOS              1
#define IP_MULTICAST_LOOP   34

#define MSG_PEEK            0x2
#define MSG_DONTWAIT        0x40

#define SOCK_RX_MAX         128             // datagrams queued before new ones get dropped
#define SOCK_DGRAM_MAX      (ETH_MTU - 28)  // one frame, only for SO_RCVBUF/SO_SNDBUF
#define SOCK_PORT_FIRST     49152
#define SOCK_PORT_LAST      65535

typedef struct __attribute__((packed)) {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t  zero[8];
} sockaddr_in_t;

typedef struct {
    int64_t sec;
    int64_t usec;
} sock_timeval_t;

// a new socket in the lowest free fd, or a negative errno
int socket_create(int domain, int type, int protocol);

// the socket behind fd, NULL with *err set when its not one
socket_t *socket_from_fd(int fd, int *err);

int  socket_bind(socket_t *s, uint32_t ip, uint16_t port);
int  socket_connect(socket_t *s, uint32_t ip, uint16_t port);
int  socket_disconnect(socket_t *s);

// buf is a user pointer for these two, ports are in host order
long socket_sendto(socket_t *s, const void *user_buf, size_t len, int flags, bool to_given, uint32_t ip, uint16_t port);
long socket_recvfrom(socket_t *s, void *user_buf, size_t len, int flags, uint32_t *ip, uint16_t *port);

void socket_name(socket_t *s, bool peer, uint32_t *ip, uint16_t *port);
int  socket_setopt(socket_t *s, int level, int name, const void *val, size_t len);
int  socket_getopt(socket_t *s, int level, int name, void *val, size_t *len);

// a sockaddr_in from the user, port comes back in host order
int sockaddr_from_user(const void *user_addr, uint64_t len, uint32_t *ip, uint16_t *port);

// write one back, user_len is in/out like the posix calls. NULL user_addr does nothing
int sockaddr_to_user(void *user_addr, uint32_t *user_len, uint32_t ip, uint16_t port);

// a udp datagram nothing in the kernel wanted, true if a socket took it
bool socket_udp_input(uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, const void *data, size_t len);
bool socket_port_used(uint16_t port);

void socket_file_release(file_t *f);
