#include <stdint.h>
#include <string.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

// the file pid was started from, so the desktop can find the icons inside it
long sys_taskpath(uint64_t pid, char *buf, uint64_t size) {
    if (!buf)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    task_t *task = sched_get_task(pid);
    if (!task)
        return -ESRCH;
    if (!task->exec_path[0])
        return -ENOENT;

    size_t len = strlen(task->exec_path) + 1;
    if (len > size)
        return -ERANGE;

    return copy_to_user(caller, buf, task->exec_path, len) != 0 ? -EFAULT : 0;
}
