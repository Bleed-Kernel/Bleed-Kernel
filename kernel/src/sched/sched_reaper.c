#include <sched/scheduler.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <sched/scheduler.h>
#include <panic.h>
#include <stdio.h>
#include <mm/spinlock.h>
#include <ipc/zero_copy.h>

#include "priv_scheduler.h"

extern task_t *task_queue;
extern task_t *task_list_head;
extern task_t *current_task;

static void unlink_from_list(task_t **head, task_t *task) {
    if (!*head || !task)
        return;

    if ((*head)->next == *head && *head == task) {
        *head = NULL;
        return;
    }

    task_t *cur = *head;
    task_t *prev = NULL;

    do {
        if (cur == task)
            break;
        prev = cur;
        cur  = cur->next;
    } while (cur != *head);

    if (cur != task)
        return;

    if (cur == *head) {
        task_t *tail = *head;
        while (tail->next != *head)
            tail = tail->next;
        *head = cur->next;
        tail->next = *head;
        return;
    }

    prev->next = cur->next;
}

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

    if (!dead_task_head) {
        dead_task_head = task;
        dead_task_tail = task;
        spinlock_release(&sched_lock);
        irq_restore(flags);
        vfs_drop(task->current_directory);
        task->current_directory = NULL;
        return;
    }

    dead_task_tail->dead_next = task;
    dead_task_tail = task;
    spinlock_release(&sched_lock);
    irq_restore(flags);

    vfs_drop(task->current_directory);
    task->current_directory = NULL;
}

void scheduler_reap(void) {
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
            if (!task) {
                spinlock_release(&sched_lock);
                irq_restore(flags);
                break;
            }

            dead_task_head = task->dead_next;
            if (!dead_task_head)
                dead_task_tail = NULL;

            if (task == current_task) {
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
                continue;
            }

            serial_printf("%sReaping Task %u\n", LOG_INFO, (unsigned int)task->id);

            if (task_queue)
                unlink_from_list(&task_queue, task);
            if (task_list_head)
                unlink_from_list(&task_list_head, task);

            uint64_t task_id = task->id;
            if (task_id > 1 && task_id < MAX_PIDS)
                pid_list[task_id] = 0;

            spinlock_release(&sched_lock);
            irq_restore(flags);

            if (task->kernel_stack)
                kfree(task->kernel_stack, KERNEL_STACK_SIZE);

            ipc_task_cleanup(task);
            paging_destroy_address_space(task->page_map);
            kfree(task, sizeof(task_t));

            reaped++;
        }

        sched_yield();
    }
    __builtin_unreachable();
}
