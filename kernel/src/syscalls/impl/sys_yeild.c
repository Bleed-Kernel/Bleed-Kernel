#include <sched/scheduler.h>
#include <stdio.h>

void sys_yield(void){
    sched_yield(get_current_task());
}