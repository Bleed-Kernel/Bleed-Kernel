#include <sched/scheduler.h>
#include <fs/vfs.h>
#include <string.h>

long sys_getcwd(char *buf, long size) {
    task_t *task = get_current_task();
    if (!task || !task->current_directory) return -1;

    INode_t *inode = task->current_directory;

    // special case: root
    stac();
    if (inode == vfs_get_root()) {
        if (size < 2) return -1;
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }
    clac();

    // Walk up to root to count total length
    size_t total_len = 0;
    INode_t *cur = inode;
    while (cur && cur != vfs_get_root()) {
        if (!cur->internal_data) return -1; // sanity check
        total_len += strlen(cur->internal_data) + 1; // +1 for '/'
        cur = cur->parent;
    }

    if (total_len + 1 > (size_t)size) return -1; // not enough space

    // Build path backwards
    stac();
    buf[total_len] = '\0';
    cur = inode;
    size_t pos = total_len;
    while (cur && cur != vfs_get_root()) {
        const char *name = cur->internal_data;
        size_t len = strlen(name);
        pos -= len;
        memcpy(buf + pos, name, len);
        pos--; // for '/'
        buf[pos] = '/';
        cur = cur->parent;
    }
    clac();

    // root case already handled, so buf should start with '/'
    return 0;
}
