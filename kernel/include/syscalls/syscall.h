#pragma once

#include <stdio.h>
#include <stdint.h>
#include <boot/sysinfo/sysinfo.h>
#include <ACPI/acpi_time.h>
#include <user/user_task.h>
#include <fs/vfs.h>
#include <user/signal.h>
#include <user/user_stat.h>

enum {
    SYS_READ            = 0,
    SYS_WRITE           = 1,
    SYS_OPEN            = 2,
    SYS_CLOSE           = 3,
    SYS_STAT            = 4,
    SYS_FSTAT           = 5,
    SYS_LSTAT           = 6,
    SYS_SEEK            = 8,
    SYS_MMAP            = 9,
    SYS_MPROTECT        = 10,
    SYS_MUNMAP          = 11,
    SYS_MADVISE         = 28,
    SYS_IOCTL           = 16,
    SYS_DUP             = 32,
    SYS_DUP2            = 33,
    SYS_FCNTL           = 72,
    SYS_FTRUNCATE       = 77,
    SYS_CLOCK_GETTIME   = 228,
    SYS_CLOCK_GETRES    = 229,
    SYS_TIMERFD_CREATE  = 283,
    SYS_EVENTFD         = 284,
    SYS_TIMERFD_SETTIME = 286,
    SYS_TIMERFD_GETTIME = 287,
    SYS_EVENTFD2        = 290,
    SYS_MEMFD_CREATE    = 319,
    SYS_UNLINKAT        = 263,
    SYS_SYMLINKAT       = 266,
    SYS_READLINKAT      = 267,
    SYS_EPOLL_CREATE    = 213,
    SYS_DUP3            = 292,
    SYS_FORK            = 57,
    SYS_VFORK           = 58,
    SYS_EXEC            = 59,
    SYS_EXIT            = 60,
    SYS_WAITPID         = 61,
    SYS_KILL            = 62,
    SYS_MKDIR           = 83,
    SYS_UNLINK          = 87,
    SYS_RENAME          = 82,
    SYS_GETCWD          = 79,
    SYS_CHDIR           = 80,
    SYS_READDIR         = 89,
    SYS_PIPE            = 293,
    SYS_GETPID          = 39,
    SYS_GETPPID         = 110,
    SYS_OPENAT          = 257,
    SYS_FSTATAT         = 262,
    SYS_POLL            = 7,
    SYS_SIGACTION       = 13,
    SYS_SIGPROCMASK     = 14,
    SYS_SIGRETURN       = 15,
    SYS_TIME            = 201,
    SYS_ARCH_PRCTL      = 158,
    SYS_EPOLL_CREATE1   = 291,
    SYS_EPOLL_CTL       = 233,
    SYS_EPOLL_WAIT      = 232,
    SYS_EPOLL_PWAIT     = 281,
    SYS_SOCKET          = 41,
    SYS_CONNECT         = 42,
    SYS_ACCEPT          = 43,
    SYS_SENDTO          = 44,
    SYS_RECVFROM        = 45,
    SYS_SENDMSG         = 46,
    SYS_RECVMSG         = 47,
    SYS_SOCK_SHUTDOWN   = 48,
    SYS_BIND            = 49,
    SYS_LISTEN          = 50,
    SYS_GETSOCKNAME     = 51,
    SYS_GETPEERNAME     = 52,
    SYS_SOCKETPAIR      = 53,
    SYS_SETSOCKOPT      = 54,
    SYS_GETSOCKOPT      = 55,
    SYS_ACCEPT4         = 288,
    SYS_GETRANDOM       = 318,
    SYS_CLONE           = 56,
    SYS_GETTID          = 186,
    SYS_TKILL           = 200,
    SYS_FUTEX           = 202,
    SYS_SET_TID_ADDRESS = 218,
    SYS_EXIT_GROUP      = 231,
    SYS_TGKILL          = 234,
    SYS_SIGALTSTACK     = 131,
    SYS_SCHED_GETAFFINITY = 204,
    SYS_PREAD64         = 17,
    SYS_PWRITE64        = 18,

    // bleed kernel specific system calls
    SYS_YIELD           = 512,
    SYS_SPAWN           = 513,
    SYS_MEMINFO         = 516,
    SYS_TASKCOUNT       = 517,
    SYS_TASKINFO        = 518,
    SYS_MAPFB           = 519,
    SYS_IPC_SEND        = 520,
    SYS_IPC_RECV        = 521,
    SYS_SHM_CREATE      = 522,
    SYS_SHM_MAP         = 523,
    SYS_SHM_DESTROY     = 524,
    SYS_TASKUSAGE       = 525,
    SYS_TASKPATH        = 526,
    SYS_SHM_MAP_AT      = 527,
    SYS_EXECVE          = 528,
    SYS_WAITSTATUS      = 529,
    SYS_MOUNTINFO       = 530,
    SYS_MOUNTLABEL      = 531,
    SYS_CPUINFO         = 532,
    SYS_WAITOPTS        = 533,
    SYS_CLOCKPARAMS     = 534,
};

// what a process needs to read the clock itself off the tsc, the same sums the kernel does.
// counts then counts_per_sec and fs_per_tick
typedef struct user_clock_params {
    uint64_t tsc_base;
    uint64_t counts_base;
    uint64_t mult;
    uint64_t counts_per_sec;
    uint64_t fs_per_tick;
    uint64_t realtime_base_ns;
} user_clock_params_t;

long sys_clockparams(uint64_t user_params);

long sys_waitopts(uint64_t pid, int *user_status, uint64_t options);
long sys_open(char *path_str, int flags);
long sys_openat(int dirfd, char *path_str, int flags);
uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len);
long sys_pipe(uint64_t user_fds_ptr, uint64_t flags);
long sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg);
long sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags);
long sys_memfd_create(uint64_t user_name, uint64_t flags);
long sys_vfork(void);
long sys_unlinkat(int dirfd, const char *user_path, int flags);
long sys_clock_gettime(uint64_t clock, uint64_t user_ts);
long sys_clock_getres(uint64_t clock, uint64_t user_ts);
long sys_eventfd(uint64_t initval);
long sys_eventfd2(uint64_t initval, uint64_t flags);
long sys_timerfd_create(uint64_t clock, uint64_t flags);
long sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t user_new, uint64_t user_old);
long sys_timerfd_gettime(uint64_t fd, uint64_t user_cur);
long sys_ftruncate(uint64_t fd, uint64_t length);
long sys_epoll_create(uint64_t size);
long sys_epoll_create1(uint64_t flags);
long sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t user_event);
long sys_epoll_wait(uint64_t epfd, uint64_t user_events, uint64_t max, uint64_t timeout);
long sys_epoll_pwait(uint64_t epfd, uint64_t user_events, uint64_t max, uint64_t timeout, uint64_t user_mask, uint64_t mask_size);
long sys_dup2(uint64_t oldfd, uint64_t newfd);
long sys_dup(uint64_t oldfd);
long sys_readdir(int fd, size_t index, dirent_t *user_ent);
long sys_unlink(const char *user_path);
long sys_rename(const char *user_oldpath, const char *user_newpath);
long sys_mkdir(const char *user_path, int mode);

long sys_close(int fd);
void sys_exit(int code);
void sys_exit_group(int code);
long sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls);
long sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3);
long sys_gettid(void);
long sys_set_tid_address(uint64_t tidptr);
long sys_tkill(long tid, long sig);
long sys_tgkill(long tgid, long tid, long sig);
void sys_yield(void);
uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg);

uint64_t sys_spawn(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc);
long sys_fork(void);
long sys_exec(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc);
long sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
long sys_mountinfo(uint64_t index, uint64_t user_info_ptr);
long sys_mountlabel(uint64_t index, uint64_t user_buf, uint64_t size);
long sys_arch_prctl(uint64_t code, uint64_t addr);
long sys_execve(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc, uint64_t user_envp_ptr);
long sys_waitpid(uint64_t pid);
long sys_waitstatus(uint64_t pid, int *user_status);

void sys_shutdown(void);
void sys_reboot(void);

long sys_kill(long pid, signal_number_t signal);
long sys_sigaction(int sig, const sigaction_t *user_act, sigaction_t *user_old);
long sys_sigaltstack(const user_stack_t *user_ss, user_stack_t *user_old);
long sys_sched_getaffinity(uint64_t pid, uint64_t len, uint8_t *user_mask);
long sys_pread64(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t offset);
long sys_pwrite64(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t offset);
long sys_sigprocmask(int how, const sigset_t *user_set, sigset_t *user_old);
long sys_sigreturn(void);
long sys_getpid(void);
long sys_getppid(void);

long sys_meminfo(system_memory_info_t *user_buf);
long sys_time(struct rtc_time* user_buf);

long sys_chdir(const char *user_path);
long sys_getcwd(char *buf, long size);

uint64_t sys_taskinfo(uint64_t pid, uint64_t user_info_ptr);
uint64_t sys_taskcount(void);
long sys_taskusage(uint64_t pid, uint64_t user_usage_ptr);
long sys_taskpath(uint64_t pid, char *buf, uint64_t size);
long sys_cpuinfo(uint64_t index, uint64_t user_info_ptr);

long sys_stat(const char *user_path, user_stat_t *user_buf);
long sys_fstatat(int dirfd, const char *user_path, user_stat_t *user_buf, int flags);
long sys_lstat(const char *user_path, user_stat_t *user_buf);
long sys_symlinkat(const char *user_target, int dirfd, const char *user_path);
long sys_readlinkat(int dirfd, const char *user_path, char *user_buf, uint64_t size);
long sys_fstat(int fd, user_stat_t *user_buf);

long sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off);
long sys_munmap(uint64_t addr, uint64_t len);
long sys_madvise(uint64_t addr, uint64_t len, uint64_t advice);

void* sys_mapfb(size_t *out_pages);

long sys_seek(int fd, long offset, int whence);
uint64_t sys_femtoseconds(void);

long sys_ipc_send(uint64_t target_pid, uint64_t src_addr, uint64_t pages);
long sys_ipc_recv(uint64_t user_msg_ptr);

long sys_shm_create(uint64_t pages);
long sys_shm_map(uint64_t id, uint64_t user_pages_ptr);
long sys_shm_map_at(uint64_t id, uint64_t first, uint64_t pages, uint64_t addr);
long sys_shm_destroy(uint64_t id);

long sys_poll(uint64_t user_fds_ptr, uint64_t nfds, long timeout_ms);

long sys_socket(uint64_t domain, uint64_t type, uint64_t protocol);
long sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t user_fds);
long sys_sendmsg(uint64_t fd, uint64_t user_msg, uint64_t flags);
long sys_recvmsg(uint64_t fd, uint64_t user_msg, uint64_t flags);
long sys_bind(uint64_t fd, uint64_t user_addr, uint64_t len);
long sys_connect(uint64_t fd, uint64_t user_addr, uint64_t len);
long sys_listen(uint64_t fd, uint64_t backlog);
long sys_accept(uint64_t fd, uint64_t user_addr, uint64_t user_len);
long sys_accept4(uint64_t fd, uint64_t user_addr, uint64_t user_len, uint64_t flags);
long sys_sock_shutdown(uint64_t fd, uint64_t how);
long sys_getrandom(uint64_t user_buf, uint64_t len, uint64_t flags);
long sys_sendto(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t flags, uint64_t user_addr, uint64_t addr_len);
long sys_recvfrom(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t flags, uint64_t user_addr, uint64_t user_addr_len);
long sys_getsockname(uint64_t fd, uint64_t user_addr, uint64_t user_len);
long sys_getpeername(uint64_t fd, uint64_t user_addr, uint64_t user_len);
long sys_setsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t user_val, uint64_t len);
long sys_getsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t user_val, uint64_t user_len);

void syscall_init(void);
