#include <stdint.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/user_task.h>
#include <user/errno.h>

long sys_taskusage(uint64_t pid, uint64_t user_usage_ptr) {
    if (!user_usage_ptr)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    user_task_usage_t usage = {0};

    // interrupts off, the task cant exit and take its page tables with it halfway through
    unsigned long flags = irq_push();

    task_t *task = sched_get_task(pid);
    if (!task) {
        irq_restore(flags);
        return -ESRCH;
    }

    usage.cpu_time = sched_task_cpu_time(task);
    if (task->task_privilege == P_USER && task->state != TASK_ZOMBIE && task->state != TASK_DEAD)
        usage.mem_pages = paging_count_user_pages(task->page_map);

    irq_restore(flags);

    if (copy_to_user(caller, (void *)user_usage_ptr, &usage, sizeof(usage)) != 0)
        return -EFAULT;
    return 0;
}
