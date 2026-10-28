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
    SYS_IOCTL           = 16,
    SYS_DUP             = 32,
    SYS_DUP2            = 33,
    SYS_FORK            = 57,
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
    SYS_SENDTO          = 44,
    SYS_RECVFROM        = 45,
    SYS_BIND            = 49,
    SYS_GETSOCKNAME     = 51,
    SYS_GETPEERNAME     = 52,
    SYS_SETSOCKOPT      = 54,
    SYS_GETSOCKOPT      = 55,

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
};

int sys_open(char *path_str, int flags);
int sys_openat(int dirfd, char *path_str, int flags);
uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len);
long sys_pipe(uint64_t user_fds_ptr);
long sys_dup2(uint64_t oldfd, uint64_t newfd);
long sys_dup(uint64_t oldfd);
long sys_readdir(int fd, size_t index, dirent_t *user_ent);
int sys_unlink(const char *user_path);
int sys_rename(const char *user_oldpath, const char *user_newpath);
int sys_mkdir(const char *user_path, int mode);

int sys_close(int fd);
void sys_exit(int code);
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
long sys_sigprocmask(int how, const sigset_t *user_set, sigset_t *user_old);
long sys_sigreturn(void);
long sys_getpid(void);
long sys_getppid(void);

int sys_meminfo(system_memory_info_t *user_buf);
int sys_time(struct rtc_time* user_buf);

long sys_chdir(const char *user_path);
long sys_getcwd(char *buf, long size);

uint64_t sys_taskinfo(uint64_t pid, uint64_t user_info_ptr);
uint64_t sys_taskcount(void);
long sys_taskusage(uint64_t pid, uint64_t user_usage_ptr);
long sys_taskpath(uint64_t pid, char *buf, uint64_t size);
long sys_cpuinfo(uint64_t index, uint64_t user_info_ptr);

long sys_stat(const char *user_path, user_stat_t *user_buf);
long sys_fstatat(int dirfd, const char *user_path, user_stat_t *user_buf, int flags);
long sys_fstat(int fd, user_stat_t *user_buf);

void *sys_mmap(size_t pages);
void sys_munmap(void *addr);

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
long sys_bind(uint64_t fd, uint64_t user_addr, uint64_t len);
long sys_connect(uint64_t fd, uint64_t user_addr, uint64_t len);
long sys_sendto(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t flags, uint64_t user_addr, uint64_t addr_len);
long sys_recvfrom(uint64_t fd, uint64_t user_buf, uint64_t len, uint64_t flags, uint64_t user_addr, uint64_t user_addr_len);
long sys_getsockname(uint64_t fd, uint64_t user_addr, uint64_t user_len);
long sys_getpeername(uint64_t fd, uint64_t user_addr, uint64_t user_len);
long sys_setsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t user_val, uint64_t len);
long sys_getsockopt(uint64_t fd, uint64_t level, uint64_t name, uint64_t user_val, uint64_t user_len);

void syscall_init(void);
