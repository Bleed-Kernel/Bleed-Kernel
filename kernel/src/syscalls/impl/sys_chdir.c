#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <status.h>
#include <mm/kalloc.h>

long sys_chdir(const char *user_path) {
    if (!user_path) return -FILE_NOT_FOUND;

    char *kbuf = str_copy_from_user(user_path, PATH_MAX);
    if (!kbuf) return -1;

    int r = vfs_chdir(kbuf);
    kfree(kbuf, PATH_MAX);
    return r;
}