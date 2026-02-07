#include <fs/vfs.h>
#include <sched/scheduler.h>

long sys_seek(int fd, long offset, int whence) {
    task_t *caller = get_current_task();
    if (!caller)
        return -1;

    return vfs_seek(fd, offset, whence);
}
