#include <sched/scheduler.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <sched/scheduler.h>
#include <panic.h>
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

void sched_mark_task_dead(void) {
    current_task->state = TASK_DEAD;
    current_task->dead_next = NULL;

    if (!dead_task_head) {
        dead_task_head = current_task;
        dead_task_tail = current_task;
        return;
    }

    dead_task_tail->dead_next = current_task;
    dead_task_tail = current_task;
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
        }
        
        sched_yield();
    }

    ke_panic("Kernel Scheduler Reaper is Dead");
}