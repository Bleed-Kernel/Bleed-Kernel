#include <mm/kalloc.h>
#include <kernel/exception/panic.h>
#include <mm/paging.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <sched/scheduler.h>
#include <ansii.h>
#include <string.h>
#include <mm/spinlock.h>
#include <fs/vfs.h>
#include <cpu/features/fpu.h>
#include <cpu/msrs.h>
#include <gdt/gdt.h>
#include <ACPI/acpi_apic.h>
#include <cpu/percpu.h>
#include <sched/bkl.h>

#include "priv_scheduler.h"

#define KERNEL_TASK_NAME    "kernel_service"

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

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

task_t *get_current_task(void) {
    return cpu_current();
}

/// @brief tsc cycles task has had the cpu for, counting the turn its in the middle of
uint64_t sched_task_cpu_time(task_t *task) {
    if (task->state != TASK_RUNNING)
        return task->cpu_time;
    return task->cpu_time + (rdtsc() - task->run_tsc);
}

static int task_prio(task_t *task) {
    int prio = (int)task->prio_base - task->prio_bonus;
    if (prio < 0)
        return 0;
    if (prio >= PRIO_LEVELS)
        return PRIO_LEVELS - 1;
    return prio;
}

static spinlock_t rq_lock;
static volatile uint64_t idle_mask; // bit n is set while cpu n sits in its idle task with nothing to do

// give a cpu thats doing nothing a reason to look at the queue
static void kick_idle_cpu(void) {
    uint64_t mask = __atomic_load_n(&idle_mask, __ATOMIC_RELAXED) & ~(1ULL << this_cpu()->id);
    if (!mask)
        return;
    int id = __builtin_ctzll(mask);
    __atomic_and_fetch(&idle_mask, ~(1ULL << id), __ATOMIC_RELAXED);
    lapic_send_ipi(cpus[id].lapic_id, SCHED_RESCHED_VECTOR);
}

static unsigned long rq_lock_irq(void) {
    unsigned long flags = irq_push();
    spinlock_acquire(&rq_lock);
    return flags;
}

static void rq_unlock_irq(unsigned long flags) {
    spinlock_release(&rq_lock);
    irq_restore(flags);
}

static void rq_enqueue(task_t *task) {
    if (task->ready_array || task->idle) return;   // already queued

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

    cpu_t *cpu = this_cpu();
    if (array == active && cpu->current &&
        (cpu->current->idle || prio < task_prio(cpu->current)))
        cpu->need_resched = 1;
}

static void rq_dequeue(task_t *task) {
    if (!task->ready_array) return;

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

void ready_enqueue(task_t *task) {
    if (!task) return;
    unsigned long flags = rq_lock_irq();
    rq_enqueue(task);
    kick_idle_cpu();
    rq_unlock_irq(flags);
}

void ready_dequeue(task_t *task) {
    if (!task) return;
    unsigned long flags = rq_lock_irq();
    rq_dequeue(task);
    rq_unlock_irq(flags);
}

int sched_has_ready(void) {
    return active->bitmap || expired->bitmap;
}

// RUNNING the moment its taken, so its cpu cant put it back in line behind our back
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
        rq_dequeue(task);

        // stopped or died while it was waiting its turn
        if (task->state == TASK_READY) {
            task->state = TASK_RUNNING;
            return task;
        }
    }
}

void sched_set_blocked(task_t *task) {
    unsigned long flags = rq_lock_irq();
    rq_dequeue(task);
    task->state = TASK_BLOCKED;
    rq_unlock_irq(flags);
}

void sched_set_running(task_t *task) {
    unsigned long flags = rq_lock_irq();
    if (task->state == TASK_BLOCKED || task->state == TASK_READY)
        task->state = TASK_RUNNING;
    rq_unlock_irq(flags);
}

void* sched_switch_task(task_t *next_task, void* old_context) {
    cpu_t  *cpu  = this_cpu();
    task_t *prev = cpu->current;
    prev->context = (cpu_context_t*)old_context;

    uint64_t now = rdtsc();
    uint64_t ran = now - prev->run_tsc;
    prev->cpu_time += ran;
    if (prev->idle)
        cpu->idle_tsc += ran;
    else
        cpu->busy_tsc += ran;
    next_task->run_tsc = now;
    cpu->switch_tsc    = now;

    // prev stays on_cpu till sched_return has us off its stack
    cpu->prev_task    = prev;
    next_task->on_cpu = 1;
    cpu_set_current(next_task);
    next_task->state = TASK_RUNNING;
    if (!next_task->idle)
        __atomic_and_fetch(&idle_mask, ~(1ULL << cpu->id), __ATOMIC_RELAXED);

    // syscall entry reads its own copy, its the same stack top
    uint64_t rsp0   = ((uint64_t)next_task->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
    cpu->tss.rsp0   = rsp0;
    cpu->kernel_rsp = rsp0;
    paging_switch_address_space(next_task->page_map);

    fpu_switch(prev, next_task);
    wrmsr(MSR_FS_BASE, next_task->fs_base);

    return (void*)next_task->context;
}

// interrupts are off in every handler that gets here
void* sched_next_context(void* old_context) {
    cpu_t  *cpu  = this_cpu();
    task_t *prev = cpu->current;
    cpu->need_resched = 0;

    spinlock_acquire(&rq_lock);

    // back in line first, it might still be the best there is
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    if (prev->state == TASK_READY)
        rq_enqueue(prev);

    task_t *next_task = ready_pop();

    // nothing at all wants the cpu. a zombie that got here must not come back to life as RUNNING
    if (!next_task) {
        if (prev->state != TASK_READY && cpu->idle_task && prev != cpu->idle_task)
            next_task = cpu->idle_task;
        else
            next_task = prev;
    }

    if (next_task == prev) {
        if (prev->state == TASK_READY)
            prev->state = TASK_RUNNING;
        spinlock_release(&rq_lock);
        return old_context;
    }

    spinlock_release(&rq_lock);

    // another cpu can still be on its stack, saving it. not under the lock
    while (__atomic_load_n(&next_task->on_cpu, __ATOMIC_ACQUIRE))
        asm volatile("pause");

    return sched_switch_task(next_task, old_context);
}

// every stub calls this with the frame its about to iret to
void sched_return(cpu_context_t *context) {
    cpu_t  *cpu  = this_cpu();
    task_t *prev = cpu->prev_task;

    if (prev) {
        cpu->prev_task = NULL;
        spinlock_acquire(&rq_lock);
        __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);

        // woken after it was already on its way out, nobody else could queue it till now
        if (prev->state == TASK_READY && !prev->ready_array) {
            rq_enqueue(prev);
            kick_idle_cpu();
        }
        spinlock_release(&rq_lock);
    }

    bkl_resume(context);
}

cpu_context_t *sched_tick(cpu_context_t *context) {
    cpu_t  *cpu  = this_cpu();
    task_t *task = cpu->current;
    if (!task) return context;

    // an idle cpu takes work the moment theres some, it has no quantum to wait out
    if (task->idle)
        return sched_has_ready() ? (cpu_context_t*)sched_next_context(context) : context;

    // only a running task gets to use up its quantum
    if (task->state == TASK_RUNNING) {
        if (task->quantum_remaining)
            task->quantum_remaining--;

        // ran a whole turn without sleeping, it gets a little less keen
        if (!task->quantum_remaining) {
            if (task->prio_bonus > -PRIO_BONUS_MAX)
                task->prio_bonus--;
        } else if (!cpu->need_resched) {
            return context;
        }
    }

    return (cpu_context_t*)sched_next_context(context);
}

// entry for the reschedule ipi, someone queued work and this cpu looked like it had none
cpu_context_t *sched_resched_handle(cpu_context_t *context) {
    task_t *task = cpu_current();
    if (!task)
        return context;
    if (task->idle ? !sched_has_ready() : !this_cpu()->need_resched)
        return context;
    return (cpu_context_t*)sched_next_context(context);
}

// entry for the yield vector, a switch without pretending a timer tick happened
cpu_context_t *sched_yield_handle(cpu_context_t *context) {
    if (!cpu_current()) return context;
    return (cpu_context_t*)sched_next_context(context);
}

__attribute__((noreturn))
static void idle_loop(void) {
    for (;;)
        sched_idle();
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

    kernel_task->run_tsc   = rdtsc();
    kernel_task->on_cpu    = 1;
    this_cpu()->switch_tsc = kernel_task->run_tsc;
    cpu_set_current(kernel_task);
    sched_queue_task_locked(kernel_task);

    // kmain is kernel code like any other task, it can end up on any cpu!
    bkl_lock();

    // so the bsp gets an idle task of its own like the aps do, first run starts it in idle_loop
    task_t *idle = sched_create_idle_task(this_cpu());
    if (!idle)
        ke_panic(NULL, "Failed to allocate the BSP idle task");

    uint64_t top = ((uint64_t)idle->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
    cpu_context_t *ctx = (cpu_context_t *)(top - sizeof(cpu_context_t));
    memset(ctx, 0, sizeof(cpu_context_t));
    ctx->rip    = (uint64_t)idle_loop;
    ctx->cs     = KERNEL_CS;
    ctx->ss     = KERNEL_SS;
    ctx->rflags = 0x202;
    ctx->rsp    = top - 8;
    idle->context = ctx;
    idle->on_cpu  = 0;

    serial_printf(LOG_OK "Kernel Task Created, tid:0\n");
}

task_t *sched_create_idle_task(cpu_t *cpu) {
    task_t *task = kmalloc(sizeof(task_t));
    if (!task)
        return NULL;
    memset(task, 0, sizeof(task_t));

    task->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack) {
        kfree(task);
        return NULL;
    }

    snprintf(task->name, sizeof(task->name), "idle_%u", cpu->id);
    task->state             = TASK_RUNNING;
    task->on_cpu            = 1;
    task->idle              = 1;
    task->quantum_remaining = QUANTUM;
    task->prio_base         = PRIO_LEVELS - 1;
    task->task_privilege    = P_KERNEL;
    task->page_map          = kernel_page_map;
    task->fd_table          = vfs_get_kernel_table();

    cpu->idle_task = task;
    return task;
}

void sched_yield(task_t *task) {
    // nothing to switch to before the scheduler exists
    if (!cpu_current()) return;

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
    task_t *self = cpu_current();

    // never held while halting
    bkl_unlock();

    // set before looking, so a wakeup in between still gets an ipi out of it
    __atomic_or_fetch(&idle_mask, 1ULL << this_cpu()->id, __ATOMIC_RELAXED);
    if (sched_has_ready()) {
        sched_yield(self);
        asm volatile("sti");
        return;
    }
    asm volatile("sti; hlt");
}

void sched_wake_task(task_t *task) {
    if (!task) return;

    unsigned long flags = rq_lock_irq();
    if (task->state == TASK_BLOCKED) {
        task->prio_bonus += PRIO_WAKE_BONUS;
        if (task->prio_bonus > PRIO_BONUS_MAX)
            task->prio_bonus = PRIO_BONUS_MAX;
        task->state = TASK_READY;
        if (!task->on_cpu) {
            rq_enqueue(task);
            kick_idle_cpu();
        }
    }
    rq_unlock_irq(flags);
}

void sched_continue_task(task_t *task) {
    if (!task) return;

    unsigned long flags = rq_lock_irq();
    if (task->state == TASK_STOPPED) {
        task->state = TASK_READY;
        if (!task->on_cpu) {
            rq_enqueue(task);
            kick_idle_cpu();
        }
    }
    rq_unlock_irq(flags);
}

void sched_block(task_t *task){
    if (!cpu_current()) return;

    unsigned long flags = irq_push();
    sched_set_blocked(task);
    asm volatile ("int $" SCHED_YIELD_VECTOR_STR);
    irq_restore(flags);
}
