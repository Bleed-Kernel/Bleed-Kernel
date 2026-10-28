#pragma once
#include <sched/scheduler.h>

// the big kernel lock. a cpu holds it whenever its running kernel code

// interrupts must be off for all of these
void bkl_lock(void);
void bkl_unlock(void);
void bkl_resume(cpu_context_t *ctx);
