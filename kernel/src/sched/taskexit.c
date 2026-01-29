#include <sched/scheduler.h>
#include <panic.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kalloc.h>
#include <ansii.h>

__attribute__((noreturn))
void exit(void) {
    int alloc_count = 0;

    task_t *current_task = get_current_task();
    task_t *w = current_task->wait_queue;

    user_alloc_t *alloc = current_task->alloc_list;
    while (alloc) {
        alloc_count++;
        user_alloc_t *next = alloc->next;

        vmm_unmap_pages(current_task->page_map, alloc->vaddr, alloc->pages);
        pmm_free_pages(vaddr_to_paddr(alloc->vaddr), alloc->pages);

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

    sched_mark_task_dead(current_task);
    sched_yield();

    serial_printf(
        "%sTask %d has exited, marking as dead for reaping and cleared %d allocations\n",
        LOG_INFO,
        current_task->id,
        alloc_count
    );
    for (;;) { asm volatile ("hlt"); }
}
