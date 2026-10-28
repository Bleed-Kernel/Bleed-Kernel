#include <sched/bkl.h>
#include <cpu/percpu.h>
#include <mm/spinlock.h>

static spinlock_t bkl;

void bkl_lock(void) {
    cpu_t *cpu = this_cpu();
    if (cpu->bkl_held)
        return;

    spinlock_acquire(&bkl);
    cpu->bkl_held = 1;
}

void bkl_unlock(void) {
    cpu_t *cpu = this_cpu();
    if (!cpu->bkl_held)
        return;

    cpu->bkl_held = 0;
    spinlock_release(&bkl);
}

void bkl_resume(cpu_context_t *ctx) {
    cpu_t *cpu = this_cpu();

    // back to ring 3 or to halting
    if ((ctx->cs & 3) || !cpu->current || cpu->current->idle)
        bkl_unlock();
    else
        bkl_lock();
}
