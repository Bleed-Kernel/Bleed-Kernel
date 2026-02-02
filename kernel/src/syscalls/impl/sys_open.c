#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <stddef.h>
#include <user/user_copy.h>
#include <sched/scheduler.h>
#include <string.h>

int sys_open(char *path_str, int flags) {
    if (!path_str) return -1;

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    task_t *caller = get_current_task();
    if (!caller) return -1;

    if (copy_from_user(caller, kpath, path_str, sizeof(kpath)) != 0)
        return -1;

    int fd = vfs_open(kpath, flags);
    return fd;
}
