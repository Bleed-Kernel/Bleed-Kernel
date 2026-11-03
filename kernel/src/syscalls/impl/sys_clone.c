#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define CLONE_VM                0x00000100
#define CLONE_FS                0x00000200
#define CLONE_FILES             0x00000400
#define CLONE_SIGHAND           0x00000800
#define CLONE_VFORK             0x00004000
#define CLONE_THREAD            0x00010000
#define CLONE_SYSVSEM           0x00040000
#define CLONE_SETTLS            0x00080000
#define CLONE_PARENT_SETTID     0x00100000
#define CLONE_CHILD_CLEARTID    0x00200000
#define CLONE_DETACHED          0x00400000
#define CLONE_CHILD_SETTID      0x01000000
#define CLONE_EXIT_SIGNAL       0x000000FF

#define CLONE_THREAD_FLAGS  (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)
#define CLONE_TID_FLAGS     (CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)
#define CLONE_THREAD_OK     (CLONE_THREAD_FLAGS | CLONE_TID_FLAGS | CLONE_SYSVSEM | CLONE_SETTLS | \
                             CLONE_DETACHED | CLONE_EXIT_SIGNAL)
#define CLONE_FORK_OK       (CLONE_TID_FLAGS | CLONE_EXIT_SIGNAL)

static int put_tid(task_t *task, uint64_t addr, uint64_t tid) {
    uint32_t t = (uint32_t)tid;
    return copy_to_user(task, (void *)addr, &t, sizeof(t)) == 0 ? 0 : -EFAULT;
}

static long clone_thread(task_t *parent, uint64_t flags, uint64_t stack, uint64_t parent_tid,
                         uint64_t child_tid, uint64_t tls) {
    if (flags & ~CLONE_THREAD_OK)
        return -EINVAL;
    if ((flags & CLONE_SETTLS) && tls && !user_ptr_valid(tls))
        return -EPERM;

    task_t *t = sched_clone_thread(parent->context, stack, (flags & CLONE_SETTLS) ? tls : parent->fs_base);
    if (!t)
        return parent->group && parent->group->exiting ? -EINTR : -ENOMEM;

    // its all one address space so the parent can fill them in, before the thread can look
    if (flags & CLONE_PARENT_SETTID)
        (void)put_tid(parent, parent_tid, t->id);
    if (flags & CLONE_CHILD_SETTID)
        (void)put_tid(parent, child_tid, t->id);
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = child_tid;

    sched_wake_task(t);
    return (long)t->id;
}

static long clone_fork(task_t *parent, uint64_t flags, uint64_t stack, uint64_t parent_tid,
                       uint64_t child_tid) {
    if (flags & ~CLONE_FORK_OK)
        return -EINVAL;

    task_t *child = sched_fork_from_context(parent->context);
    if (!child)
        return -ENOMEM;

    if (stack)
        child->context->rsp = stack;
    if (flags & CLONE_PARENT_SETTID)
        (void)put_tid(parent, parent_tid, child->id);
    if (flags & CLONE_CHILD_SETTID)
        (void)put_tid(child, child_tid, child->id);
    if (flags & CLONE_CHILD_CLEARTID)
        child->clear_child_tid = child_tid;

    sched_wake_task(child);
    return (long)child->id;
}

long sys_clone(uint64_t flags, uint64_t stack, uint64_t parent_tid, uint64_t child_tid, uint64_t tls) {
    task_t *parent = get_current_task();
    if (!parent || parent->task_privilege != P_USER)
        return -ESRCH;

    if (stack && !user_ptr_valid(stack))
        return -EFAULT;

    if ((flags & CLONE_THREAD_FLAGS) == CLONE_THREAD_FLAGS)
        return clone_thread(parent, flags, stack, parent_tid, child_tid, tls);

    // TODO: vfork and sharing only some of it, posix_spawn wants CLONE_VM | CLONE_VFORK
    if (flags & (CLONE_THREAD_FLAGS | CLONE_VFORK))
        return -EINVAL;

    return clone_fork(parent, flags, stack, parent_tid, child_tid);
}
