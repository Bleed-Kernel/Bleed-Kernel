#include <mm/userspace/mmap.h>
#include <sched/scheduler.h>

void sys_munmap(void *addr){
    return task_munmap(get_current_task(), addr);
}