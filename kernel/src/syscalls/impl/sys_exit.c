#include <threads/exit.h>
#include <stdio.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>

void sys_exit(int code){
    get_current_task()->exit_code = code & 0xFF;
    exit();
}