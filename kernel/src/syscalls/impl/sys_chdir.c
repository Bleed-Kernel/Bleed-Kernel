#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <status.h>
#include <mm/kalloc.h>
#include <string.h>

long sys_chdir(const char *user_path) {
    if (!user_path) return -FILE_NOT_FOUND;

    char kbuf[PATH_MAX];
    memset(kbuf, 0, PATH_MAX);

    task_t *caller = get_current_task();
    if (!caller) return -1;

    if (copy_from_user(caller, kbuf, user_path, PATH_MAX) != 0)
        return -1;

    int r = vfs_chdir(kbuf);
    return r;
}
