#include <mm/kalloc.h>
#include <panic.h>
#include <mm/paging.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <ansii.h>
#include <tss/tss.h>
#include <string.h>
#include <mm/spinlock.h>

#include "priv_scheduler.h"

#define KERNEL_TASK_NAME    "bleed kernel"

struct task_t;

task_t *current_task   = NULL;
task_t *task_queue     = NULL;
task_t *task_list_head = NULL;

task_t *dead_task_head = NULL;
task_t *dead_task_tail = NULL;

task_t *get_current_task() {
    return current_task;
}

void init_scheduler(void) {
    asm volatile ("cli");
    task_queue = task_list_head;
    asm volatile ("sti");
}

void* sched_switch_task(task_t *next_task, void* old_context) {
    current_task->context = (cpu_context_t*)old_context;
    FP_Save(current_task->fx_state);

    if (current_task->state == TASK_RUNNING) {
        current_task->state = TASK_READY;
    }


    current_task = next_task;
    current_task->state = TASK_RUNNING;
    current_task->quantum_remaining = QUANTUM;

    tss.rsp0 = (uint64_t)current_task->kernel_stack + KERNEL_STACK_SIZE;
    paging_switch_address_space(current_task->page_map);
    FP_Restore(current_task->fx_state);

    return (void*)next_task->context;
}

void* sched_next_context(void* old_context) {
    task_t *next_task = current_task->next;

    while (next_task->state != TASK_READY && next_task != current_task) {
        next_task = next_task->next;
    }

    return sched_switch_task(next_task, old_context);
}

cpu_context_t *sched_tick(cpu_context_t *context) {
    if (!current_task) return context;

    if (current_task->quantum_remaining > 0) {
        current_task->quantum_remaining--;
        return context;
    }

    current_task->quantum_remaining = QUANTUM;
    return (cpu_context_t*)sched_next_context(context);
}

void sched_bootstrap(void *rsp) {
    task_t *kernel_task = kmalloc(sizeof(task_t));
    if (!kernel_task)
        ke_panic("Failed to allocate kernel task");

    memset(kernel_task, 0, sizeof(task_t));

    kernel_task->id                 = 0;
    kernel_task->state              = TASK_RUNNING;
    kernel_task->quantum_remaining  = QUANTUM;
    kernel_task->context            = (cpu_context_t *)rsp;
    kernel_task->next               = kernel_task;
    kernel_task->task_privilege     = P_KERNEL;
    FP_Init(kernel_task->fx_state);

    strncpy(kernel_task->name, KERNEL_TASK_NAME, 128-1);
    kernel_task->page_map = kernel_page_map;

    current_task   = kernel_task;
    task_queue     = kernel_task;
    task_list_head = kernel_task;

    serial_printf(LOG_OK "Kernel Task Created, tid:0\n");
}

void sched_yield(void) {
    asm volatile ("cli");
    if (current_task && current_task->state == TASK_RUNNING) {
        current_task->quantum_remaining = 0;
        current_task->state = TASK_READY;
    }
    asm volatile ("sti");
    asm volatile ("int $32");
}
