#include <mm/userspace/mmap.h>
#include <sched/scheduler.h>

void sys_munmap(void *addr){
    task_munmap(get_current_task(), addr);
    return;
}