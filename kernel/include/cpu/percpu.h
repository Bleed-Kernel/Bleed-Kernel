#pragma once

// offsets the entry stubs use through gs, keep in step with cpu_t
#define CPU_SELF            0
#define CPU_KERNEL_RSP      8
#define CPU_USER_RSP        16
#define CPU_CURRENT         24

#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
#include <gdt/gdt.h>
#include <tss/tss.h>

#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

#define MAX_CPUS            64

#define CPU_STR_(x)         #x
#define CPU_STR(x)          CPU_STR_(x)

struct task;

typedef struct cpu {
    struct cpu      *self;
    uint64_t        kernel_rsp;     // top of currents kernel stack, same as
    uint64_t        user_rsp;       // syscall entry parks the rsp here
    struct task     *current;

    uint32_t        id;
    uint32_t        lapic_id;
    volatile uint8_t online;        // set by the cpu itself, 1 once its scheduling, 2 if it couldnt
    uint8_t         need_resched;   // someone with a higher priority got requeued so stand aside
    uint8_t         bkl_held;
    struct task     *prev_task;     // switched away from but still on its stack till sched_return
    struct task     *idle_task;     // runs only when nothing else can
    uint64_t        switch_tsc;
    uint64_t        busy_tsc;
    uint64_t        idle_tsc;

    gdt_t           gdt __attribute__((aligned(16)));
    tss_t           tss __attribute__((aligned(16)));
} cpu_t;

_Static_assert(offsetof(cpu_t, self)       == CPU_SELF,       "CPU_SELF");
_Static_assert(offsetof(cpu_t, kernel_rsp) == CPU_KERNEL_RSP, "CPU_KERNEL_RSP");
_Static_assert(offsetof(cpu_t, user_rsp)   == CPU_USER_RSP,   "CPU_USER_RSP");
_Static_assert(offsetof(cpu_t, current)    == CPU_CURRENT,    "CPU_CURRENT");

extern cpu_t    cpus[MAX_CPUS];
extern uint32_t cpu_count;

// point gs at the bsps cpu_t we gotta do this before we ask for what the current task is
void cpu_bsp_early_init(void);

static inline cpu_t *this_cpu(void) {
    cpu_t *cpu;
    asm volatile("mov %%gs:" CPU_STR(CPU_SELF) ", %0" : "=r"(cpu));
    return cpu;
}

static inline struct task *cpu_current(void) {
    struct task *task;
    asm volatile("mov %%gs:" CPU_STR(CPU_CURRENT) ", %0" : "=r"(task));
    return task;
}

static inline void cpu_set_current(struct task *task) {
    asm volatile("mov %0, %%gs:" CPU_STR(CPU_CURRENT) :: "r"(task) : "memory");
}
#endif
