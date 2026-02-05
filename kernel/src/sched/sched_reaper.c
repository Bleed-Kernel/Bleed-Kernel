#include <sched/scheduler.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <sched/scheduler.h>
#include <panic.h>
#include <stdio.h>

#include "priv_scheduler.h"

extern task_t *task_queue;
extern task_t *task_list_head;
extern task_t *current_task;

static void unlink_from_list(task_t **head, task_t *task) {
    if (!*head || !task)
        return;

    if (*head == task && (*head)->next == *head) {
        *head = NULL;
        return;
    }

    task_t *prev = *head;
    task_t *cur  = (*head)->next;

    while (cur != *head) {
        if (cur == task) {
            prev->next = cur->next;
            return;
        }
        prev = cur;
        cur  = cur->next;
    }

    if (*head == task)
        *head = (*head)->next;
}

void sched_mark_task_dead(task_t *task) {
    if (!task){
        kprintf(LOG_ERROR "Nothing Happend, This task does not exist\n");
        return;
    }

    if (task->id == 0 || task->id == 1)
        kprintf(LOG_WARN "You have killed a Supervisor Task, the system may behave unpredictably\n run the 'reboot' command to restart your machine\n");

    task->state = TASK_DEAD;
    task->dead_next = NULL;

    if (!dead_task_head) {
        dead_task_head = task;
        dead_task_tail = task;
        return;
    }

    dead_task_tail->dead_next = task;
    dead_task_tail = task;

    vfs_drop(task->current_directory);
}

void scheduler_reap(void) {
    for (;;) {
        int reaped = 0;

        while (dead_task_head && reaped < 15) {
            task_t *task = dead_task_head;
            if (!task) break;

            dead_task_head = task->dead_next;
            if (!dead_task_head)
                dead_task_tail = NULL;

            if (task == current_task)
                continue;

            serial_printf("%sReaping Task %u\n", LOG_INFO, (unsigned int)task->id);

            if (task_queue)
                unlink_from_list(&task_queue, task);
            if (task_list_head)
                unlink_from_list(&task_list_head, task);

            if (task->kernel_stack)
                kfree(task->kernel_stack, KERNEL_STACK_SIZE);

            paging_destroy_address_space(task->page_map);
            kfree(task, sizeof(task_t));

            reaped++;
            if (task->id > 1 && task->id < MAX_PIDS)
                pid_list[task->id] = 0;
        }

        sched_yield();
    }
    __builtin_unreachable();
}