#include <sched/scheduler.h>
#include <kernel/exception/panic.h>
#include <stdio.h>
#include <mm/vmm.h>
#include <mm/kalloc.h>
#include <sched/signal.h>
#include <sched/futex.h>
#include <user/user_copy.h>
#include <ipc/shm.h>
#include <devices/type/fb_device.h>
#include <devices/type/tty_device.h>
#include <devices/type/audio_device.h>

#include "priv_scheduler.h"

// pthread_join sleeps on this
static void clear_child_tid(task_t *task) {
    uint64_t addr = task->clear_child_tid;
    task->clear_child_tid = 0;
    if (!addr)
        return;

    uint32_t zero = 0;
    if (copy_to_user(task, (void *)addr, &zero, sizeof(zero)) == 0)
        futex_wake(task, addr, 1, FUTEX_BITSET_MATCH_ANY, 0);
}

// the last thread out takes the process down with it
__attribute__((noreturn))
static void process_exit(task_t *current_task) {
    task_group_t *g      = current_task->group;
    task_t       *leader = g && g->leader ? g->leader : current_task;
    task_t       *w      = leader->wait_queue;

    if (g && g->exiting) {
        leader->exit_code   = g->exit_code;
        leader->exit_signal = g->exit_signal;
    }

    // done here and not in the reaper, the screen should come back the moment its owner is gone
    fb_device_task_exit(current_task);
    tty_task_exit(current_task);
    audio_device_task_exit(current_task);
    shm_task_cleanup(current_task);

    user_alloc_t *alloc = g ? g->alloc_list : NULL;
    while (alloc) {
        user_alloc_t *next = alloc->next;

        (void)vmm_unmap_free_pages(current_task->page_map, alloc->vaddr, alloc->pages);

        user_alloc_free(alloc);
        alloc = next;
    }
    if (g)
        g->alloc_list = NULL;

    // the rest goes now too, a zombie can sit a long time before anyone waits on it
    if (current_task->task_privilege == PRIVILEGE_USER) {
        paging_release_user_space(current_task->page_map);
        paging_switch_address_space(current_task->page_map);
    }
    vfs_fd_table_drop(current_task->fd_table);
    current_task->fd_table = NULL;

    while (w) {
        task_t *next = w->wait_next;
        w->wait_next = NULL;
        sched_wake_task(w);
        w = next;
    }

    leader->wait_queue = NULL;

    // our children outlive us. they go to the reaper, and any that already exited were
    // only being kept around for a wait we are never going to make
    task_t *orphan;
    while ((orphan = sched_reparent_children(leader)))
        sched_mark_task_dead(orphan);

    // a zombie has no use for where it was, and holding on keeps that filesystem busy
    INode_t *cwd = current_task->current_directory;
    current_task->current_directory = NULL;
    vfs_drop(cwd);

    task_t *parent = NULL;
    if (leader->ppid > 0)
        parent = sched_get_task(leader->ppid);

    if (parent && parent->task_privilege == PRIVILEGE_USER) {
        signal_send_group(parent, SIGCHLD);

        // SIGCHLD only lands on one thread, it might not be the one sat in waitpid
        if (parent->group) {
            for (task_t *t = parent->group->threads; t; t = t->group_next) {
                if (t->wait_target_pid == leader->id || t->wait_target_pid == WAIT_ANY_CHILD)
                    sched_wake_task(t);
            }
        }
    }

    // only a live user task can wait for us. a kernel parent (the reaper, for an orphan)
    // never will, so theres no point in leaving a zombie behind for it
    if (!parent || parent->task_privilege != PRIVILEGE_USER || !task_group_alive(parent)) {
        sched_mark_task_dead(leader);
    } else {
        leader->state = TASK_ZOMBIE;
    }

    if (current_task != leader)
        sched_mark_task_dead(current_task);

    for (;;)
        sched_yield(get_current_task());
}

__attribute__((noreturn))
void thread_exit(void) {
    task_t       *current_task = get_current_task();
    task_group_t *g            = current_task->group;

    vfork_release(current_task);
    if (!g || g->live <= 1) {
        if (g)
            g->live = 0;
        process_exit(current_task);
    }

    clear_child_tid(current_task);
    g->live--;

    vfs_fd_table_drop(current_task->fd_table);
    current_task->fd_table = NULL;

    INode_t *cwd = current_task->current_directory;
    current_task->current_directory = NULL;
    vfs_drop(cwd);

    // the leader hangs about as a zombie so the parent still has the pid to wait on
    if (current_task == g->leader)
        current_task->state = TASK_ZOMBIE;
    else
        sched_mark_task_dead(current_task);

    for (;;)
        sched_yield(get_current_task());
}

__attribute__((noreturn))
void exit(void) {
    task_t       *current_task = get_current_task();
    task_group_t *g            = current_task->group;

    if (g && !g->exiting) {
        g->exiting     = 1;
        g->exit_code   = current_task->exit_code;
        g->exit_signal = current_task->exit_signal;

        for (task_t *t = g->threads; t; t = t->group_next) {
            if (t != current_task)
                signal_send(t, SIGKILL);
        }
    }

    thread_exit();
}
