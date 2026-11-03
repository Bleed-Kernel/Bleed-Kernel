#include <sched/bkl.h>
#include <cpu/percpu.h>
#include <mm/tlb.h>

// tickets, first come first served
static volatile uint32_t bkl_next;
static volatile uint32_t bkl_serving;

void bkl_lock(void) {
    cpu_t *cpu = this_cpu();
    if (cpu->bkl_held)
        return;

    uint32_t ticket = __atomic_fetch_add(&bkl_next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&bkl_serving, __ATOMIC_ACQUIRE) != ticket) {
        // the holder might be waiting on us to flush, and our irqs are off
        tlb_flush_check();
        asm volatile("pause");
    }
    cpu->bkl_held = 1;
}

void bkl_unlock(void) {
    cpu_t *cpu = this_cpu();
    if (!cpu->bkl_held)
        return;

    cpu->bkl_held = 0;
    __atomic_store_n(&bkl_serving, bkl_serving + 1, __ATOMIC_RELEASE);
}

void bkl_resume(cpu_context_t *ctx) {
    cpu_t *cpu = this_cpu();

    // back to ring 3 or to halting
    if ((ctx->cs & 3) || !cpu->current || cpu->current->idle)
        bkl_unlock();
    else
        bkl_lock();
}
