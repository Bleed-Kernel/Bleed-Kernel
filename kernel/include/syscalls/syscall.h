#pragma once

#include <stdio.h>
#include <stdint.h>
#include <boot/sysinfo/sysinfo.h>
#include <ACPI/acpi_time.h>
#include <user/user_task.h>
#include <fs/vfs.h>
#include <user/signal.h>

int sys_open(char *path_str, int flags);
uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len);
long sys_readdir(int fd, size_t index, dirent_t *user_ent);

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
int sys_time(struct rtc_time* user_buf);

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
void* sys_mapfb(task_t *task, size_t *out_pages);

long sys_seek(int fd, long offset, int whence);
uint64_t sys_femtoseconds();
