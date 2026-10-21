#include <mm/kalloc.h>
#include <kernel/exception/panic.h>
#include <mm/paging.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <ansii.h>
#include <tss/tss.h>
#include <string.h>
#include <mm/spinlock.h>
#include <fs/vfs.h>
#include <cpu/features/fpu.h>

#include "priv_scheduler.h"

#define KERNEL_TASK_NAME    "bleed_kernel"

task_t *current_task   = NULL;
task_t *task_list_head = NULL;

task_t *dead_task_head = NULL;
task_t *dead_task_tail = NULL;

// fifo of tasks waiting for the cpu, O(1) at both ends
task_t *ready_head     = NULL;
task_t *ready_tail     = NULL;

// when the current task was given the cpu
static uint64_t switch_tsc = 0;

static task_t *idle_task = NULL;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

task_t *get_current_task(void) {
    return current_task;
}

/// @brief tsc cycles task has had the cpu for, counting the turn its in the middle of
uint64_t sched_task_cpu_time(task_t *task) {
    if (task != current_task)
        return task->cpu_time;
    return task->cpu_time + (rdtsc() - switch_tsc);
}

void ready_enqueue(task_t *task) {
    if (!task || task->ready_queued || task == idle_task) return;   // already queued

    task->ready_next = NULL;
    if (ready_tail)
        ready_tail->ready_next = task;
    else
        ready_head = task;

    ready_tail         = task;
    task->ready_queued = 1;
}

// only for a task thats about to be freed. one that just blocks stays where it is and
// ready_pop throws it out when it gets to the front, so blocking never walks the queue
void ready_dequeue(task_t *task) {
    if (!task || !task->ready_queued) return;

    task_t *prev = NULL;
    task_t *cur  = ready_head;
    while (cur && cur != task) {
        prev = cur;
        cur  = cur->ready_next;
    }
    if (!cur) return;

    if (prev)
        prev->ready_next = cur->ready_next;
    else
        ready_head = cur->ready_next;

    if (ready_tail == cur)
        ready_tail = prev;

    task->ready_next   = NULL;
    task->ready_queued = 0;
}

static task_t *ready_pop(void) {
    while (ready_head) {
        task_t *task = ready_head;

        ready_head = task->ready_next;
        if (!ready_head)
            ready_tail = NULL;

        task->ready_next   = NULL;
        task->ready_queued = 0;

        // blocked, stopped or died while it was waiting its turn
        if (task->state == TASK_READY)
            return task;
    }
    return NULL;
}

void* sched_switch_task(task_t *next_task, void* old_context) {
    current_task->context = (cpu_context_t*)old_context;

    uint64_t now = rdtsc();
    current_task->cpu_time += now - switch_tsc;
    switch_tsc = now;

    if (current_task->state == TASK_RUNNING)
        current_task->state = TASK_READY;
    if (current_task->state == TASK_READY)
        ready_enqueue(current_task);       // back onto ready queue

    current_task = next_task;
    current_task->state = TASK_RUNNING;
    current_task->quantum_remaining = QUANTUM;

    tss.rsp0 = ((uint64_t)current_task->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
    paging_switch_address_space(current_task->page_map);

    // the fpu registers are left alone, the next task traps if it wants them
    fpu_switch(current_task);

    return (void*)next_task->context;
}

void* sched_next_context(void* old_context) {
    task_t *next_task = ready_pop();

    // nothing else wants the cpu, carry on with what we have. the state is only touched if
    // it was a plain yield, a zombie that got here must not come back to life as RUNNING
    if (!next_task) {
        int can_run = current_task->state == TASK_READY || current_task->state == TASK_RUNNING;
        if (!can_run && idle_task && current_task != idle_task)
            return sched_switch_task(idle_task, old_context);

        if (current_task->state == TASK_READY)
            current_task->state = TASK_RUNNING;
        current_task->quantum_remaining = QUANTUM;
        return old_context;
    }

    return sched_switch_task(next_task, old_context);
}

cpu_context_t *sched_tick(cpu_context_t *context) {
    if (!current_task) return context;

    // only a running task gets to use up its quantum
    if (current_task->state == TASK_RUNNING && current_task->quantum_remaining > 0) {
        current_task->quantum_remaining--;
        return context;
    }

    return (cpu_context_t*)sched_next_context(context);
}

// entry for the yield vector, a switch without pretending a timer tick happened
cpu_context_t *sched_yield_handle(cpu_context_t *context) {
    if (!current_task) return context;
    return (cpu_context_t*)sched_next_context(context);
}

void sched_bootstrap(void *rsp) {
    task_t *kernel_task = kmalloc(sizeof(task_t));
    if (!kernel_task)
        ke_panic(NULL, "Failed to allocate kernel task");

    memset(kernel_task, 0, sizeof(task_t));

    kernel_task->id                 = 0;
    kernel_task->ppid               = 0;
    kernel_task->pgid               = 0;
    kernel_task->sid                = 0;
    kernel_task->state              = TASK_RUNNING;
    kernel_task->quantum_remaining  = QUANTUM;
    kernel_task->context            = (cpu_context_t *)rsp;
    kernel_task->next               = kernel_task;
    kernel_task->ready_next         = NULL;
    kernel_task->task_privilege     = P_KERNEL;
    if (fpu_task_init(kernel_task) != 0)
        ke_panic(NULL, "Failed to allocate kernel task FPU state");

    strncpy(kernel_task->name, KERNEL_TASK_NAME, 128-1);
    kernel_task->page_map = kernel_page_map;
    kernel_task->fd_table = vfs_get_kernel_table();
    if (!kernel_task->fd_table)
        ke_panic(NULL, "Failed to allocate kernel fd table");

    current_task   = kernel_task;
    task_list_head = kernel_task;
    switch_tsc     = rdtsc();

    serial_printf(LOG_OK "Kernel Task Created, tid:0\n");
}

void sched_yield(task_t *task) {
    // nothing to switch to before the scheduler exists
    if (!current_task) return;

    // the callers interrupt state is theirs to decide, put it back how we found it
    unsigned long flags = irq_push();
    if (task && task->state == TASK_RUNNING) {
        task->quantum_remaining = 0;
        task->state = TASK_READY;
    }
    asm volatile ("int $" SCHED_YIELD_VECTOR_STR);
    irq_restore(flags);
}

/// @brief one turn of the idle loop, hands the cpu over if anyone is waiting for it or halts till an irq
void sched_idle(void) {
    asm volatile("cli");
    idle_task = current_task;
    if (ready_head) {
        sched_yield(current_task);
        asm volatile("sti");
        return;
    }
    asm volatile("sti; hlt");
}

void sched_block(task_t *task){
    if (!current_task) return;

    unsigned long flags = irq_push();
    task->state = TASK_BLOCKED;
    asm volatile ("int $" SCHED_YIELD_VECTOR_STR);
    irq_restore(flags);
}
