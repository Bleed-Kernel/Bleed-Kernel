#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <user/errno.h>
#include <drivers/serial/serial.h>

// theres no shared address space under a vfork, the child gets a cow copy like fork and the
// parent just waits till its exec'd or gone. a program can only exec or _exit there anyway
typedef struct vfork_done {
    volatile int done;
    int          refs;
    wait_queue_t wait;
} vfork_done_t;

static void vfork_put(vfork_done_t *vd) {
    if (--vd->refs > 0)
        return;
    sched_wait_queue_destroy(&vd->wait);
    kfree(vd);
}

void vfork_release(task_t *task) {
    vfork_done_t *vd = task->vfork;
    if (!vd)
        return;
    task->vfork = NULL;
    vd->done = 1;
    sched_wake_queue(&vd->wait);
    vfork_put(vd);
}

long sys_fork(void) {
    task_t *parent = get_current_task();
    if (!parent)
        return -ESRCH;

    task_t *child = sched_fork_from_context(parent->context);
    if (!child) {
        return -ENOMEM;
    }

    sched_wake_task(child);
    return (long)child->id;
}

// the child already made and not woken, stack is where it starts if given
long vfork_start(task_t *parent, task_t *child) {
    vfork_done_t *vd = kmalloc(sizeof(vfork_done_t));
    if (vd) {
        vd->done = 0;
        vd->refs = 2;
        vd->wait.head = NULL;
        child->vfork = vd;
    }
    long pid = (long)child->id;
    sched_wake_task(child);
    if (!vd)
        return pid;

    // a signal lets the parent off early, nothing is shared so thats harmless
    while (!vd->done && !signal_should_interrupt(parent)) {
        sigset_t seen = parent->sig_pending;
        unsigned long flags = irq_push();
        if (!vd->done && parent->sig_pending == seen)
            sched_wait_queue(&vd->wait, 0);
        irq_restore(flags);
    }
    vfork_put(vd);
    return pid;
}

long sys_vfork(void) {
    task_t *parent = get_current_task();
    if (!parent)
        return -ESRCH;

    task_t *child = sched_fork_from_context(parent->context);
    if (!child)
        return -ENOMEM;
    return vfork_start(parent, child);
}
