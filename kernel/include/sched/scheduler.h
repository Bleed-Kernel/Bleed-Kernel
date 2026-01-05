#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stddef.h>
#include <mm/paging.h>

#define KERNEL_STACK_SIZE   8196

#define MAX_TASKS           64
#define QUANTUM             10

typedef struct cpu_context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error;
    uint64_t rip, cs, rflags, rsp, ss;
} cpu_context_t;

typedef enum {
    TASK_FREE,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint64_t        id;
    task_state_t    state;
    cpu_context_t  *context;

    uint8_t *kernel_stack;
    uint32_t quantum_remaining;

    paddr_t page_map;

    struct task *parent;
    struct task *waiting_parent;


    struct task *next;
    struct task *dead_next;
} task_t;

typedef void (*task_itteration_fn)(task_t *task, void *userdata);

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss);
extern void sched_bootstrap(void *rsp);
extern cpu_context_t *sched_tick(cpu_context_t *context);
void scheduler_reap(void);
void sched_mark_task_dead();
task_t *sched_get_task(uint64_t pid);

// api
const char *task_state_str(task_state_t state);
task_t *get_task_by_id(uint64_t tid);
uint64_t get_task_count();
task_t *get_current_task();
void sched_yield(void);

void itterate_each_task(task_itteration_fn fn, void *userdata);

extern task_t *task_list_head;
#endif