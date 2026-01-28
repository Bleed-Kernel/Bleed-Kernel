#pragma once

#include <stdio.h>
#include <stdint.h>
#include <boot/sysinfo/sysinfo.h>
#include <ACPI/acpi_time.h>
#include <user/user_task.h>
#include <fs/vfs.h>

int sys_open(char *path_str, int flags);
uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len);
long sys_readdir(int fd, size_t index, dirent_t *user_ent);

int sys_close(int fd);
void sys_exit();
uint64_t sys_clear(uint64_t fd);
void sys_yield();

uint64_t sys_spawn(uint64_t user_path_ptr);
long sys_waitpid(uint64_t pid);

void sys_shutdown(void);
void sys_reboot(void);

void sys_tkill(long pid);

system_memory_info_t *sys_meminfo();
int sys_time(struct rtc_time* user_buf);

uintptr_t sys_alloc(uint64_t pages);
uintptr_t sys_free(uint64_t addr, uint64_t pages);

long sys_chdir(const char *user_path);
long sys_getcwd(char *buf, size_t size);

int sys_taskinfo(uint64_t pid, user_task_info_t *user_task);
int sys_taskcount(uint64_t *user_count);