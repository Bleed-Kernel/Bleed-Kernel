#include <mm/userspace/mmap.h>
#include <sched/scheduler.h>

void *sys_mmap(size_t pages){
    return task_mmap(get_current_task(), pages);
}
