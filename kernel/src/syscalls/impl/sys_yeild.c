#include <sched/scheduler.h>
#include <stdio.h>

void sys_yield(){
    sched_yield(get_current_task());
}