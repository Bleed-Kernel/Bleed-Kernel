#include <net/socket.h>
#include <syscalls/syscall.h>

long sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    return socket_create((int)domain, (int)type, (int)protocol);
}
