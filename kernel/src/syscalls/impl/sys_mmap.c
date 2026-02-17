#include <mm/userspace/mmap.h>
#include <sched/scheduler.h>
#include <user/errno.h>

void *sys_mmap(size_t pages){
    if (pages == 0)
        return (void *)(uintptr_t)-EINVAL;

    task_t *task = get_current_task();
    if (!task)
        return (void *)(uintptr_t)-ESRCH;

    void *addr = task_mmap(task, pages);
    if (!addr)
        return (void *)(uintptr_t)-ENOMEM;
    return addr;
}
