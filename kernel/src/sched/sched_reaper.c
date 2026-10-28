#include <sched/scheduler.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <kernel/exception/panic.h>
#include <stdio.h>
#include <mm/spinlock.h>
#include <ipc/zero_copy.h>
#include <cpu/features/fpu.h>

#include "priv_scheduler.h"

static task_t *reaper_task = NULL;

void sched_mark_task_dead(task_t *task) {
    if (!task){
        kprintf(LOG_ERROR "Nothing Happend, This task does not exist\n");
        return;
    }

    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    if (task->state == TASK_DEAD) {
        spinlock_release(&sched_lock);
        irq_restore(flags);
        return;
    }

    if (task->id == 0 || task->id == 1)
        kprintf(LOG_WARN "You have killed a Supervisor Task, the system may behave unpredictably\n run the 'reboot' command to restart your machine\n");

    task->state = TASK_DEAD;
    task->dead_next = NULL;
    ready_dequeue(task);

    __typeof__(task->current_directory) cwd = task->current_directory;
    task->current_directory = NULL;

    if (!dead_task_head) {
        dead_task_head = task;
        dead_task_tail = task;
    } else {
        dead_task_tail->dead_next = task;
        dead_task_tail = task;
    }

    sched_wake_task(reaper_task);

    spinlock_release(&sched_lock);
    irq_restore(flags);

    vfs_drop(cwd);
}

void scheduler_reap(void) {
    reaper_task = get_current_task();
    reaper_task->prio_base = PRIO_REAPER;

    for (;;) {
        int reaped = 0;

        while (reaped < 15) {
            unsigned long flags = irq_push();
            spinlock_acquire(&sched_lock);

            if (!dead_task_head) {
                spinlock_release(&sched_lock);
                irq_restore(flags);
                break;
            }

            task_t *task = dead_task_head;
            dead_task_head = task->dead_next;
            if (!dead_task_head)
                dead_task_tail = NULL;

            // still running or switching out on some cpu
            if (task->on_cpu) {
                task->dead_next = NULL;
                if (dead_task_tail) {
                    dead_task_tail->dead_next = task;
                    dead_task_tail = task;
                } else {
                    dead_task_head = task;
                    dead_task_tail = task;
                }
                spinlock_release(&sched_lock);
                irq_restore(flags);
                break;
            }

            sched_wait_cleanup(task);
            sched_unlink_task_locked(task);

            spinlock_release(&sched_lock);
            irq_restore(flags);

            fpu_task_free(task);

            if (task->kernel_stack)
                kfree(task->kernel_stack);

            if (task->heap)
                kfree(task->heap);

            // a task killed before it reached exit() still owns these, exit() NULLs them when it got there first
            vfs_fd_table_drop(task->fd_table);
            sched_free_alloc_list(task->alloc_list);

            ipc_task_cleanup(task);
            paging_destroy_address_space(task->page_map);
            kfree(task);

            reaped++;
        }

        // nothing left to do
        unsigned long flags = irq_push();
        if (dead_task_head) {
            irq_restore(flags);
            sched_yield(reaper_task);
            continue;
        }
        sched_block(reaper_task);
        irq_restore(flags);
    }
    __builtin_unreachable();
}