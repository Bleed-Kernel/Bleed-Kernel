#include <threads/exit.h>
#include <stdio.h>
#include <syscalls/syscall.h>
#include <sched/scheduler.h>

void sys_exit(){
    task_t *current_task = get_current_task();

    if (current_task->waiting_parent){
        current_task->waiting_parent->state = TASK_READY;
    }

    exit();
}