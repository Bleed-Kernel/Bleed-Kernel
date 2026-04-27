// note to self. these files are getting really, really messy i gotta clean this up
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mm/paging.h>
#include <mm/userspace/mmap.h>
#include <fs/vfs.h>
#include <user/signal.h>
#include <ipc/epoll.h>

#define KERNEL_STACK_SIZE   8196

#define USER_STACK_TOP      0x00007ffffffff000ULL
#define USER_STACK_SIZE     (8 * 1024 * 1024)

#define MAX_TASKS           64
#define QUANTUM             10

#define FP_Save(buf) \
    __asm__ volatile ( \
        "fxsave64 (%0)" \
        : : "r"(buf) : "memory" \
    )

#define FP_Restore(buf) \
    __asm__ volatile ( \
        "fxrstor64 (%0)" \
        : : "r"(buf) : "memory" \
    )

#define FP_Init(buf) \
    __asm__ volatile ( \
        "fninit\n\t" \
        "fxsave64 (%0)" \
        : : "r"(buf) : "memory" \
    )

typedef struct user_heap user_heap_t;
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
    task_state_t    state;
    cpu_context_t  *context;

    uint8_t         *kernel_stack;
    uint32_t        quantum_remaining;

    paddr_t         page_map;
    user_alloc_t    *alloc_list;
    user_heap_t     *heap;
    fd_table_t      *fd_table;

    INode_t         *current_directory;
    task_privil_t   task_privilege;

    uint8_t         fx_state[512] __attribute__((aligned(16)));

    struct task     *wait_queue;
    struct task     *wait_next;
    struct task     *ready_next;
    uint64_t        wait_target_pid;

    struct task     *next;
    struct task     *dead_next;

    sigset_t        sig_pending;
    sigset_t        sig_blocked;
    uintptr_t       sig_handlers[NSIG];
    sigset_t        sig_masks[NSIG];
    uint64_t        sig_flags[NSIG];
    uintptr_t       sig_restorers[NSIG];
    uintptr_t       sig_active_frame;

    poll_table_t    ipc_poll;

    int             exit_signal;
    int             exit_code;

    uint8_t         tty_suspended;
    uint8_t         tty_suspend_was_runnable;

    ipc_message_t   *ipc_head;
    ipc_message_t   *ipc_tail;
} task_t;

typedef void (*task_itteration_fn)(task_t *task, void *userdata);

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss, char *task_name);
task_t *sched_fork_from_context(cpu_context_t *parent_ctx);
extern void sched_bootstrap(void *rsp);
extern cpu_context_t *sched_tick(cpu_context_t *context);
void scheduler_reap(void);
void sched_mark_task_dead(task_t *task);
task_t *sched_get_task(uint64_t pid);

uint64_t get_task_count();
task_t *get_current_task();
void sched_yield(task_t *task);
void sched_block(task_t *task);
void sched_wake(task_t *task);

void itterate_each_task(task_itteration_fn fn, void *userdata);

void* task_mmap(task_t* task, size_t pages);
void task_munmap(task_t* task, void* addr);
void sched_init_task_heap(task_t* task);
void* sched_switch_task(task_t *next_task, void* old_context);
void* sched_next_context(void* old_context);

cpu_context_t *sched_kill_and_switch(task_t *victim);

extern task_t *task_list_head;
