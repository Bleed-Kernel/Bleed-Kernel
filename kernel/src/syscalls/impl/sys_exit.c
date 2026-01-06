#include <threads/exit.h>
#include <stdio.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>

void sys_exit(){
    exit();
}