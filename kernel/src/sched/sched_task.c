#include <sched/scheduler.h>
#include <panic.h>
#include <mm/heap.h>
#include <string.h>
#include <mm/paging.h>
#include <stdio.h>

#include "priv_scheduler.h"

#define USER_STACK_TOP 0x00007ffffffff000ULL
#define USER_STACK_SIZE 16384

extern task_t *task_queue;
extern task_t *task_list_head;
static uint64_t next_pid = 1;

static void queue_task(task_t *task) {
    if (!task_queue) {
        task_queue = task;
        task->next = task;
        return;
    }

    task_t *tail = task_queue;
    while (tail->next != task_queue)
        tail = tail->next;

    tail->next = task;
    task->next = task_queue;
}

task_t *sched_get_task(uint64_t pid) {
    if (!task_list_head) return NULL;

    task_t *t = task_list_head;
    do {
        if (t->id == pid)
            return t;
        t = t->next;
    } while (t != task_list_head);

    return NULL;
}

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss) {
    task_t *task = kmalloc(sizeof(task_t));
    if (!task) ke_panic("Failed to allocate task");

    task->id = next_pid++;
    task->state = TASK_READY;
    task->quantum_remaining = QUANTUM;
    task->page_map = cr3;

    // Kernel stack
    task->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack)
        ke_panic("Failed to allocate kernel stack");
    uint64_t kernel_stack_top = (uint64_t)task->kernel_stack + KERNEL_STACK_SIZE;

    for (uint64_t page = USER_STACK_TOP - USER_STACK_SIZE; page < USER_STACK_TOP; page += PAGE_SIZE) {
        paddr_t paddr = pmm_alloc_pages(1);
        if (!paddr) ke_panic("Failed to allocate user stack page");

        paging_map_page(task->page_map, paddr, page, PTE_USER | PTE_WRITABLE);
    }

    // CPU context on kernel stack
    cpu_context_t *ctx = (cpu_context_t *)(kernel_stack_top - sizeof(cpu_context_t));
    memset(ctx, 0, sizeof(cpu_context_t));

    ctx->rip = entry;
    ctx->cs  = cs;
    ctx->ss  = ss;
    ctx->rflags = 0x202;

    // Choose stack depending on user/kernel code segment
    ctx->rsp = (cs & 0x3) ? USER_STACK_TOP : kernel_stack_top;

    task->context = ctx;

    // Queue the task
    queue_task(task);
    if (!task_list_head) task_list_head = task;

    return task;
}

void itterate_each_task(task_itteration_fn fn, void *userdata) {
    if (!task_list_head || !fn)
        return;

    task_t *task = task_list_head;
    do {
        fn(task, userdata);
        task = task->next;
    } while (task != task_list_head);
}

uint64_t get_task_count(void) {
    if (!task_list_head)
        return 0;

    uint64_t count = 0;
    task_t *task = task_list_head;

    do {
        count++;
        task = task->next;
    } while (task != task_list_head);

    return count;
}
