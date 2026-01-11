#include <stdio.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <ansii.h>

void sys_tkill(long pid) {
    sched_mark_task_dead(sched_get_task(pid));
}