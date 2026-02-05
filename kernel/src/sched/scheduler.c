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

cpu_context_t *sched_tick(cpu_context_t *context) {
    if (!current_task)
        return context;

    current_task->context = context;

    AVX_Save(current_task->avx_state);

    if (current_task->quantum_remaining > 0)
        current_task->quantum_remaining--;

    if (current_task->quantum_remaining > 0)
        return context;

    current_task->quantum_remaining = QUANTUM;

    if (current_task->state == TASK_RUNNING)
        current_task->state = TASK_READY;

    task_t *task = current_task->next;
    task_t *start = task;

    do {
        if (task->state == TASK_READY) {
            current_task = task;
            current_task->state = TASK_RUNNING;

            tss.rsp0 = (uint64_t)current_task->kernel_stack + KERNEL_STACK_SIZE;
            paging_switch_address_space(current_task->page_map);

            AVX_Restore(current_task->avx_state);

            return current_task->context;
        }
        task = task->next;
    } while (task != start);

    current_task->state = TASK_RUNNING;
    return current_task->context;
}

void* sched_switch_context(void* old_context) {
    if (current_task->state == TASK_RUNNING) {
        current_task->state = TASK_READY;
    }
    
    current_task->context = (cpu_context_t*)old_context;
    AVX_Save(current_task->avx_state);

    task_t *next_task = current_task->next;
    while (next_task->state != TASK_READY && next_task != current_task) {
        next_task = next_task->next;
    }

    current_task = next_task;
    current_task->state = TASK_RUNNING;
    current_task->quantum_remaining = QUANTUM;

    tss.rsp0 = (uint64_t)current_task->kernel_stack + KERNEL_STACK_SIZE;
    paging_switch_address_space(current_task->page_map);
    AVX_Restore(current_task->avx_state);
    
    return (void*)current_task->context;
}

void sched_bootstrap(void *rsp) {
    task_t *kernel_task = kmalloc(sizeof(task_t));
    if (!kernel_task)
        ke_panic("Failed to allocate kernel task");

    kernel_task->id                 = 0;
    kernel_task->state              = TASK_RUNNING;
    kernel_task->quantum_remaining  = QUANTUM;
    kernel_task->context            = (cpu_context_t *)rsp;
    kernel_task->next               = kernel_task;
    kernel_task->task_privilege     = P_KERNEL;
    kernel_task->avx_state[0]       = 0;

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
