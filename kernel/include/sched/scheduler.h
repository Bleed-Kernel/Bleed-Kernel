// note to self. these files are getting really, really messy i gotta clean this up
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mm/paging.h>
#include <mm/userspace/mmap.h>
#include <fs/vfs.h>
#include <user/signal.h>

#define KERNEL_STACK_SIZE   8196

#define USER_STACK_TOP      0x00007ffffffff000ULL
#define USER_STACK_SIZE     16384

#define MAX_TASKS           64
#define QUANTUM             10

#define AVX_Save(buf) \
    __asm__ volatile ( \
        "vmovdqu %%ymm0,  0(%0)\n\t" \
        "vmovdqu %%ymm1, 32(%0)\n\t" \
        "vmovdqu %%ymm2, 64(%0)\n\t" \
        "vmovdqu %%ymm3, 96(%0)\n\t" \
        "vmovdqu %%ymm4, 128(%0)\n\t" \
        "vmovdqu %%ymm5, 160(%0)\n\t" \
        "vmovdqu %%ymm6, 192(%0)\n\t" \
        "vmovdqu %%ymm7, 224(%0)\n\t" \
        "vmovdqu %%ymm8, 256(%0)\n\t" \
        "vmovdqu %%ymm9, 288(%0)\n\t" \
        "vmovdqu %%ymm10, 320(%0)\n\t" \
        "vmovdqu %%ymm11, 352(%0)\n\t" \
        "vmovdqu %%ymm12, 384(%0)\n\t" \
        "vmovdqu %%ymm13, 416(%0)\n\t" \
        "vmovdqu %%ymm14, 448(%0)\n\t" \
        "vmovdqu %%ymm15, 480(%0)" \
        : : "r"(buf) : "memory" \
    )

#define AVX_Restore(buf) \
    __asm__ volatile ( \
        "vmovdqu  0(%0), %%ymm0\n\t" \
        "vmovdqu 32(%0), %%ymm1\n\t" \
        "vmovdqu 64(%0), %%ymm2\n\t" \
        "vmovdqu 96(%0), %%ymm3\n\t" \
        "vmovdqu 128(%0), %%ymm4\n\t" \
        "vmovdqu 160(%0), %%ymm5\n\t" \
        "vmovdqu 192(%0), %%ymm6\n\t" \
        "vmovdqu 224(%0), %%ymm7\n\t" \
        "vmovdqu 256(%0), %%ymm8\n\t" \
        "vmovdqu 288(%0), %%ymm9\n\t" \
        "vmovdqu 320(%0), %%ymm10\n\t" \
        "vmovdqu 352(%0), %%ymm11\n\t" \
        "vmovdqu 384(%0), %%ymm12\n\t" \
        "vmovdqu 416(%0), %%ymm13\n\t" \
        "vmovdqu 448(%0), %%ymm14\n\t" \
        "vmovdqu 480(%0), %%ymm15" \
        : : "r"(buf) : "memory" \
    )

typedef struct user_heap user_heap_t;

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
    char            name[128];
    task_state_t    state;
    cpu_context_t  *context;

    uint8_t         *kernel_stack;
    uint32_t        quantum_remaining;

    paddr_t         page_map;
    user_alloc_t    *alloc_list;
    user_heap_t     *heap;

    INode_t         *current_directory;
    task_privil_t   task_privilege;

    uint8_t avx_state[512] __attribute__((aligned(32)));

    struct task     *wait_queue;
    struct task     *wait_next;

    struct task     *next;
    struct task     *dead_next;

    sigset_t        sig_pending;
    sigset_t        sig_blocked;
    uintptr_t       sig_handlers[NSIG];
    sigset_t        sig_masks[NSIG];
    uint64_t        sig_flags[NSIG];
    uintptr_t       sig_restorers[NSIG];
    uintptr_t       sig_active_frame;

    int             exit_signal;
    int             exit_code;
} task_t;

typedef void (*task_itteration_fn)(task_t *task, void *userdata);

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss, char *task_name);
extern void sched_bootstrap(void *rsp);
extern cpu_context_t *sched_tick(cpu_context_t *context);
void scheduler_reap(void);
void sched_mark_task_dead(task_t *task);
task_t *sched_get_task(uint64_t pid);

uint64_t get_task_count();
task_t *get_current_task();
void sched_yield(void);

void itterate_each_task(task_itteration_fn fn, void *userdata);

void* task_mmap(task_t* task, size_t pages);
void task_munmap(task_t* task, void* addr);
void sched_init_task_heap(task_t* task);
void* sched_switch_task(task_t *next_task, void* old_context);
void* sched_next_context(void* old_context);

cpu_context_t *sched_kill_and_switch(task_t *victim);

extern task_t *task_list_head;
