#pragma once

#include <stdio.h>
#include <stdint.h>
#include <boot/sysinfo/sysinfo.h>
#include <ACPI/acpi_time.h>
#include <user/user_task.h>
#include <fs/vfs.h>
#include <user/signal.h>
#include <ipc/epoll.h>

enum {
    SYS_READ            = 0,
    SYS_WRITE           = 1,
    SYS_OPEN            = 2,
    SYS_CLOSE           = 3,
    SYS_STAT            = 4,
    SYS_SEEK            = 8,
    SYS_MMAP            = 9,
    SYS_MUNMAP          = 11,
    SYS_IOCTL           = 16,
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
    SYS_SIGACTION       = 13,
    SYS_SIGPROCMASK     = 14,
    SYS_SIGRETURN       = 15,
    SYS_TIME            = 201,
    SYS_EPOLL_CREATE1   = 291,
    SYS_EPOLL_CTL       = 233,
    SYS_EPOLL_WAIT      = 232,
    SYS_EPOLL_PWAIT     = 281,

    // bleed kernel specific system calls
    SYS_YIELD           = 512,
    SYS_SPAWN           = 513,
    SYS_SHUTDOWN        = 514,
    SYS_REBOOT          = 515,
    SYS_MEMINFO         = 516,
    SYS_TASKCOUNT       = 517,
    SYS_TASKINFO        = 518,
    SYS_MAPFB           = 519,
    SYS_IPC_SEND        = 520,
    SYS_IPC_RECV        = 521,
};

int sys_open(char *path_str, int flags);
uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len);
long sys_pipe(uint64_t user_fds_ptr);
long sys_dup2(uint64_t oldfd, uint64_t newfd);
long sys_readdir(int fd, size_t index, dirent_t *user_ent);
int sys_unlink(const char *user_path);
int sys_rename(const char *user_oldpath, const char *user_newpath);
int sys_mkdir(const char *user_path, int mode);

int sys_close(int fd);
void sys_exit();
uint64_t sys_clear(uint64_t fd);
void sys_yield();
uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg);

uint64_t sys_spawn(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc);
long sys_fork(void);
long sys_exec(uint64_t user_path_ptr, uint64_t user_argv_ptr, uint64_t user_argc);
long sys_waitpid(uint64_t pid);

void sys_shutdown(void);
void sys_reboot(void);

long sys_kill(long pid, signal_number_t signal);
long sys_sigaction(int sig, const sigaction_t *user_act, sigaction_t *user_old);
long sys_sigprocmask(int how, const sigset_t *user_set, sigset_t *user_old);
long sys_sigreturn(void);
long sys_getpid(void);

system_memory_info_t *sys_meminfo();
int sys_time(struct rtc_time *user_buf);

uintptr_t sys_alloc(uint64_t pages);
uintptr_t sys_free(uint64_t addr, uint64_t pages);

long sys_chdir(const char *user_path);
long sys_getcwd(char *buf, size_t size);

uint64_t sys_taskinfo(uint64_t pid, uint64_t user_info_ptr);
uint64_t sys_taskcount(void);

int sys_stat(int fd, user_file_t *user_buf);

void *sys_mmap(size_t pages);
void sys_munmap(void *addr);

int sys_test_usercopy(uint64_t user_buf_ptr, uint64_t len);
void *sys_mapfb(task_t *task, size_t *out_pages);

long sys_seek(int fd, long offset, int whence);
uint64_t sys_femtoseconds();

long sys_ipc_send(uint64_t target_pid, uint64_t src_addr, uint64_t pages);
long sys_ipc_recv(uint64_t user_msg_ptr);

long sys_epoll_create1(int flags);
long sys_epoll_ctl(int epfd, int op, int fd, epoll_event_t *user_ev);
long sys_epoll_wait(int epfd, epoll_event_t *user_events, int maxevents, int timeout_ms);
long sys_epoll_pwait(int epfd, epoll_event_t *user_events, int maxevents, int timeout_ms,
                     const uint64_t *user_sigmask, size_t sigsetsize);
void epoll_fd_close(task_t *task, int fd);

void syscall_init(void);