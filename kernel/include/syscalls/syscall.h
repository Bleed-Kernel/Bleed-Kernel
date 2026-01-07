#pragma once

#include <stdio.h>
#include <stdint.h>

uint64_t sys_read(uint64_t fd, uint64_t user_buf, uint64_t len);
uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len);
void sys_exit();
uint64_t sys_clear(uint64_t fd);
void sys_yield();

uint64_t sys_spawn(uint64_t user_path_ptr);
long sys_waitpid(uint64_t pid);

void sys_shutdown(void);
void sys_reboot(void);