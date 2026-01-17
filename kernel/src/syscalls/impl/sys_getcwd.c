#include <stdint.h>
#include <sched/scheduler.h>
#include <string.h>

long sys_getcwd(char *buf, long size) {
    task_t *task = get_current_task();
    if (!task || !task->current_directory) return -1;

    if (task->current_directory == vfs_get_root()) {
        if (size < 2) return -1;
        buf[0] = '/';
        buf[1] = 0;
        return 0;
    }

    const char *name = task->current_directory->internal_data;
    long len = strlen(name);
    if (len + 2 > size) return -1;

    buf[0] = '/';
    strcpy(buf + 1, name);
    return 0;
}