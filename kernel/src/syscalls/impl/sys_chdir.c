#include <fs/vfs.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <mm/kalloc.h>
#include <string.h>
#include <user/errno.h>

long sys_chdir(const char *user_path) {
    if (!user_path)
        return -EFAULT;

    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;

    // PATH_MAX is half of an 8K kernel stack before the vfs has even been called, keep it on the heap
    char *kbuf = kmalloc(PATH_MAX);
    if (!kbuf)
        return -ENOMEM;

    long plen = copy_user_path(caller, user_path, kbuf, PATH_MAX);
    if (plen < 0) {
        kfree(kbuf);
        return plen == -2 ? -E2BIG : -EFAULT;
    }

    int r = vfs_chdir(kbuf);
    kfree(kbuf);
    if (r != 0 || !caller->group)
        return r;

    // the working directory is the processes, not the threads
    for (task_t *t = caller->group->threads; t; t = t->group_next) {
        if (t == caller || !t->current_directory || !caller->current_directory)
            continue;

        INode_t *old = t->current_directory;
        t->current_directory = caller->current_directory;
        t->current_directory->shared++;
        vfs_drop(old);
    }

    return r;
}
