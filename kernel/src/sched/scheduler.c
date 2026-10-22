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

struct prio_array {
    uint32_t bitmap;                // bit n is set while level n has someone in it
    task_t  *head[PRIO_LEVELS];
    task_t  *tail[PRIO_LEVELS];
};

// a task that used up its quantum goes to expired, the two swap once active runs dry.
static prio_array_t  prio_arrays[2];
static prio_array_t *active  = &prio_arrays[0];
static prio_array_t *expired = &prio_arrays[1];

// when expired last went from empty to not
static uint64_t expired_since = 0;
extern volatile uint64_t timer_ticks;

// someone better than current_task got queued, the next tick hands the cpu over
static uint8_t need_resched = 0;

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

static int task_prio(task_t *task) {
    int prio = (int)task->prio_base - task->prio_bonus;
    if (prio < 0)
        return 0;
    if (prio >= PRIO_LEVELS)
        return PRIO_LEVELS - 1;
    return prio;
}

void ready_enqueue(task_t *task) {
    if (!task || task->ready_array || task == idle_task) return;   // already queued

    prio_array_t *array = active;
    if (!task->quantum_remaining) {
        task->quantum_remaining = QUANTUM;
        array = expired;
        if (!expired->bitmap)
            expired_since = timer_ticks;
    }

    int prio = task_prio(task);
    task->ready_prev = array->tail[prio];
    task->ready_next = NULL;
    if (array->tail[prio])
        array->tail[prio]->ready_next = task;
    else
        array->head[prio] = task;

    array->tail[prio]  = task;
    array->bitmap     |= 1u << prio;
    task->ready_array  = array;
    task->ready_prio   = (uint8_t)prio;

    if (array == active && current_task &&
        (current_task == idle_task || prio < task_prio(current_task)))
        need_resched = 1;
}

void ready_dequeue(task_t *task) {
    if (!task || !task->ready_array) return;

    prio_array_t *array = task->ready_array;
    int prio = task->ready_prio;

    if (task->ready_prev)
        task->ready_prev->ready_next = task->ready_next;
    else
        array->head[prio] = task->ready_next;

    if (task->ready_next)
        task->ready_next->ready_prev = task->ready_prev;
    else
        array->tail[prio] = task->ready_prev;

    if (!array->head[prio])
        array->bitmap &= ~(1u << prio);

    task->ready_prev  = NULL;
    task->ready_next  = NULL;
    task->ready_array = NULL;
}

int sched_has_ready(void) {
    return active->bitmap || expired->bitmap;
}

static task_t *ready_pop(void) {
    for (;;) {
        int starving = expired->bitmap && timer_ticks - expired_since >= STARVE_TICKS;
        if (!active->bitmap || starving) {
            prio_array_t *swap = active;
            active  = expired;
            expired = swap;
            expired_since = timer_ticks;
        }
        if (!active->bitmap)
            return NULL;

        task_t *task = active->head[__builtin_ctz(active->bitmap)];
        ready_dequeue(task);

        // stopped or died while it was waiting its turn
        if (task->state == TASK_READY)
            return task;
    }
}

void* sched_switch_task(task_t *next_task, void* old_context) {
    current_task->context = (cpu_context_t*)old_context;

    uint64_t now = rdtsc();
    current_task->cpu_time += now - switch_tsc;
    switch_tsc = now;

    current_task = next_task;
    current_task->state = TASK_RUNNING;

    tss.rsp0 = ((uint64_t)current_task->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
    paging_switch_address_space(current_task->page_map);

    // the fpu registers are left alone, the next task traps if it wants them
    fpu_switch(current_task);

    return (void*)next_task->context;
}

void* sched_next_context(void* old_context) {
    task_t *prev = current_task;
    need_resched = 0;

    // back in line first, it might still be the best there is
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    if (prev->state == TASK_READY)
        ready_enqueue(prev);

    task_t *next_task = ready_pop();

    // nothing at all wants the cpu. a zombie that got here must not come back to life as RUNNING
    if (!next_task) {
        if (prev->state != TASK_READY && idle_task && prev != idle_task)
            return sched_switch_task(idle_task, old_context);
        next_task = prev;
    }

    if (next_task == prev) {
        if (prev->state == TASK_READY)
            prev->state = TASK_RUNNING;
        return old_context;
    }

    return sched_switch_task(next_task, old_context);
}

cpu_context_t *sched_tick(cpu_context_t *context) {
    if (!current_task) return context;

    // only a running task gets to use up its quantum
    if (current_task->state == TASK_RUNNING) {
        if (current_task->quantum_remaining)
            current_task->quantum_remaining--;

        // ran a whole turn without sleeping, it gets a little less keen
        if (!current_task->quantum_remaining) {
            if (current_task->prio_bonus > -PRIO_BONUS_MAX)
                current_task->prio_bonus--;
        } else if (!need_resched) {
            return context;
        }
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
    kernel_task->prio_base          = PRIO_DEFAULT;
    kernel_task->task_privilege     = P_KERNEL;
    if (fpu_task_init(kernel_task) != 0)
        ke_panic(NULL, "Failed to allocate kernel task FPU state");

    strncpy(kernel_task->name, KERNEL_TASK_NAME, 128-1);
    kernel_task->page_map = kernel_page_map;
    kernel_task->fd_table = vfs_get_kernel_table();
    if (!kernel_task->fd_table)
        ke_panic(NULL, "Failed to allocate kernel fd table");

    current_task = kernel_task;
    sched_queue_task_locked(kernel_task);
    switch_tsc   = rdtsc();

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
    if (sched_has_ready()) {
        sched_yield(current_task);
        asm volatile("sti");
        return;
    }
    asm volatile("sti; hlt");
}

void sched_wake_task(task_t *task) {
    if (!task || task->state != TASK_BLOCKED) return;

    task->prio_bonus += PRIO_WAKE_BONUS;
    if (task->prio_bonus > PRIO_BONUS_MAX)
        task->prio_bonus = PRIO_BONUS_MAX;

    task->state = TASK_READY;
    ready_enqueue(task);
}

void sched_block(task_t *task){
    if (!current_task) return;

    unsigned long flags = irq_push();
    task->state = TASK_BLOCKED;
    asm volatile ("int $" SCHED_YIELD_VECTOR_STR);
    irq_restore(flags);
}
