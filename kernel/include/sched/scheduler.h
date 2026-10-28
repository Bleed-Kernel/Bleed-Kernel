// note to self. these files are getting really, really messy i gotta clean this up
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mm/paging.h>
#include <mm/userspace/mmap.h>
#include <fs/vfs.h>
#include <user/signal.h>
#include <sched/wait_queue.h>

#define KERNEL_STACK_SIZE   8192

#define USER_STACK_TOP      0x00007ffffffff000ULL
#define USER_STACK_SIZE     (8 * 1024 * 1024)

#define QUANTUM             10
#define STARVE_TICKS        (QUANTUM * 2)   // longest anything sits in expired before the arrays swap anyway

// 0 is the highest
#define PRIO_LEVELS         32
#define PRIO_DEFAULT        16
#define PRIO_REAPER         24
#define PRIO_AUDIO          8
#define PRIO_BONUS_MAX      4
#define PRIO_WAKE_BONUS     2

// orphans are handed to the reaper, the same job init has on linux
#define REAPER_PID          1

// software interrupt sched_yield uses to give the cpu away
#define SCHED_YIELD_VECTOR      0x81
#define SCHED_YIELD_VECTOR_STR  "0x81"

// another cpu wants this one to look at the run queue
#define SCHED_RESCHED_VECTOR    0xF0

typedef struct user_heap user_heap_t;
typedef struct prio_array prio_array_t;
typedef struct ipc_message ipc_message_t;

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
    TASK_STOPPED,
    TASK_ZOMBIE,
    TASK_DEAD
} task_state_t;

typedef struct user_alloc {
    void* vaddr;
    size_t pages;
    struct user_alloc* next;
} user_alloc_t;

#define P_KERNEL    0
#define P_USER      1
typedef enum {
    PRIVILEGE_KERNEL,
    PRIVILEGE_USER
} task_privil_t;

typedef struct task {
    uint64_t        id;
    uint64_t        ppid;
    uint64_t        pgid;
    uint64_t        sid;
    char            name[128];
    char            exec_path[256];
    task_state_t    state;
    cpu_context_t  *context;

    uint8_t         *kernel_stack;
    uint32_t        quantum_remaining;
    uint64_t        cpu_time;       // tsc cycles spent on the cpu, up to the last switch away
    uint64_t        run_tsc;        // when it was last given the cpu

    paddr_t         page_map;
    user_alloc_t    *alloc_list;
    user_heap_t     *heap;
    fd_table_t      *fd_table;

    INode_t         *current_directory;
    task_privil_t   task_privilege;

    uint8_t         *fpu_state;      // xsave area, only current while someone else owns the fpu

    struct task     *wait_queue;
    struct task     *wait_next;
    uint64_t        wait_target_pid;

    uint8_t         on_cpu;         // running, or still being switched out, somewhere
    uint8_t         idle;           // a cpus idle task, never queued

    uint8_t         prio_base;
    int8_t          prio_bonus;     // up for sleeping, down for using whole quanta
    uint8_t         ready_prio;     // the level its queued at
    prio_array_t    *ready_array;   // NULL when its not queued
    struct task     *ready_prev;
    struct task     *ready_next;

    wait_entry_t    *wait_entries;  // what its sleeping on, only set while it is
    size_t          wait_entry_count;
    uint64_t        event_deadline; // hpet count to give up at, 0 = no timeout
    uint8_t         deadline_queued;
    struct task     *deadline_prev;
    struct task     *deadline_next;

    struct task     *next;
    struct task     *prev;
    struct task     *dead_next;
    struct task     *pid_next;

    struct task     *parent;
    struct task     *children;
    struct task     *sibling_prev;
    struct task     *sibling_next;

    sigset_t        sig_pending;
    sigset_t        sig_blocked;
    uintptr_t       sig_handlers[NSIG];
    sigset_t        sig_masks[NSIG];
    uint64_t        sig_flags[NSIG];
    uintptr_t       sig_restorers[NSIG];
    uintptr_t       sig_active_frame;

    uint64_t        fs_base;

    int             exit_signal;
    int             exit_code;

    uint8_t         tty_suspended;
    uint8_t         tty_suspend_was_runnable;

    ipc_message_t   *ipc_head;
    ipc_message_t   *ipc_tail;
    wait_queue_t    ipc_wait;
} task_t;

typedef void (*task_itteration_fn)(task_t *task, void *userdata);

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss, const char *task_name);
task_t *sched_fork_from_context(cpu_context_t *parent_ctx);
extern void sched_bootstrap(void *rsp);
extern cpu_context_t *sched_tick(cpu_context_t *context);
void scheduler_reap(void);
void sched_mark_task_dead(task_t *task);
task_t *sched_get_task(uint64_t pid);

uint64_t get_task_count(void);
uint64_t sched_task_cpu_time(task_t *task);
task_t *get_current_task(void);
void sched_yield(task_t *task);
void sched_block(task_t *task);

// a BLOCKED task can run again, and gets a bump for having slept
void sched_wake_task(task_t *task);

// a STOPPED task can run again
void sched_continue_task(task_t *task);

// one turn of a cpus idle task, runs only when nothing else can
void sched_idle(void);

struct cpu;

// an idle task for an ap, never on the task list so nothing but that cpu ever runs it
task_t *sched_create_idle_task(struct cpu *cpu);

// sleep till the deadline or a signal. interrupts must be off
void sched_sleep_until(uint64_t deadline);
void sched_event_tick(void);
uint64_t sched_deadline_ms(uint64_t ms);
int sched_deadline_passed(uint64_t deadline);
task_t *sched_reparent_children(task_t *parent);

void itterate_each_task(task_itteration_fn fn, void *userdata);

void* task_mmap(task_t* task, size_t pages);
void task_munmap(task_t* task, void* addr);
void* task_mmap_reserve(task_t* task, size_t pages);
void* task_mmap_reserve_at(task_t* task, uintptr_t addr, size_t pages);
void task_mmap_release(task_t* task, void* addr);
void sched_free_alloc_list(user_alloc_t *list);
void sched_init_task_heap(task_t* task);
void* sched_switch_task(task_t *next_task, void* old_context);
void* sched_next_context(void* old_context);

void exit(void);

extern task_t *task_list_head;
