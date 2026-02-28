#include <sched/scheduler.h>
#include <panic.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <mm/vmm.h>
#include <mm/kalloc.h>
#include <ansii.h>
#include <sched/signal.h>

__attribute__((noreturn))
void exit(void) {
    int alloc_count = 0;

    task_t *current_task = get_current_task();
    task_t *w = current_task->wait_queue;

    user_alloc_t *alloc = current_task->alloc_list;
    while (alloc) {
        alloc_count++;
        user_alloc_t *next = alloc->next;

        (void)vmm_unmap_free_pages(current_task->page_map, alloc->vaddr, alloc->pages);

        kfree(alloc, sizeof(user_alloc_t));
        alloc = next;
    }
    current_task->alloc_list = NULL;

    while (w) {
        task_t *next = w->wait_next;

        w->wait_next = NULL;
        w->state = TASK_READY;
        w = next;
    }

    current_task->wait_queue = NULL;

    task_t *parent = NULL;
    if (current_task->ppid > 0)
        parent = sched_get_task(current_task->ppid);

    if (parent && parent->task_privilege == PRIVILEGE_USER)
        signal_send(parent, SIGCHLD);

    if (!parent || parent->state == TASK_DEAD || parent->state == TASK_ZOMBIE || parent->state == TASK_FREE) {
        sched_mark_task_dead(current_task);
    } else {
        current_task->state = TASK_ZOMBIE;
    }

    sched_yield();

    serial_printf(
        "%sTask %d has exited, marking as dead for reaping and cleared %d allocations\n",
        LOG_INFO,
        current_task->id,
        alloc_count
    );
    for (;;) { asm volatile ("hlt"); }
}
