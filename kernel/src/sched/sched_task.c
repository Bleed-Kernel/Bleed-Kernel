#include <sched/scheduler.h>
#include <panic.h>
#include <mm/kalloc.h>
#include <string.h>
#include <mm/paging.h>
#include <stdio.h>
#include <ansii.h>
#include <mm/spinlock.h>

#include "priv_scheduler.h"

extern task_t *task_queue;
extern task_t *task_list_head;

uint8_t pid_list[MAX_PIDS] = {0};
spinlock_t sched_lock;

__attribute__((constructor))
void sched_init_lock(void) {
    spinlock_init(&sched_lock);
}

int alloc_pid(){
    int pid = -1;
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);

    for (int i = 1; i < MAX_PIDS; i++){
        if (!pid_list[i]){
            pid_list[i] = 1;
            pid = i;
            break;
        }
    }

    spinlock_release(&sched_lock);
    irq_restore(flags);
    return pid;
}

static void queue_task(task_t *task) {
    if (!task_list_head) {
        task_list_head = task;
        task->next = task;
        return;
    }

    task_t *tail = task_list_head;
    while (tail->next != task_list_head)
        tail = tail->next;

    tail->next = task;
    task->next = task_list_head;
}

task_t *sched_get_task(uint64_t pid) {
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);

    if (!task_list_head) {
        spinlock_release(&sched_lock);
        irq_restore(flags);
        return NULL;
    }

    task_t *t = task_list_head;
    do {
        if (t->id == pid){
            spinlock_release(&sched_lock);
            irq_restore(flags);
            return t;
        }
        t = t->next;
    } while (t != task_list_head);

    spinlock_release(&sched_lock);
    irq_restore(flags);
    return NULL;
}

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss, char *task_name) {
    task_t *task = kmalloc(sizeof(task_t));
    if (!task) ke_panic("Failed to allocate task");
    memset(task, 0, sizeof(task_t));

    uint64_t pid = alloc_pid();
    if (pid > 0) task->id = pid;
    
    // SID is basically future facing semantics, i do intend on using it
    task_t *parent = get_current_task();
    if (parent && parent->id != 0) {
        task->ppid = parent->id;
        task->pgid = parent->pgid ? parent->pgid : parent->id;
        task->sid = parent->sid ? parent->sid : parent->id;
    } else if (parent) {
        task->ppid = parent->id;
        task->pgid = task->id;
        task->sid = task->id;
    } else {
        task->ppid = 0;
        task->pgid = task->id;
        task->sid = task->id; 
    }

    if (task_name) {
        strncpy(task->name, task_name, sizeof(task->name) - 1);
        task->name[sizeof(task->name) - 1] = '\0';
    }
    task->state = TASK_READY;
    task->quantum_remaining = QUANTUM;
    task->page_map = cr3;

    task->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack)
        ke_panic("Failed to allocate kernel stack");
    uint64_t kernel_stack_top = (uint64_t)task->kernel_stack + KERNEL_STACK_SIZE;

    for (uint64_t page = USER_STACK_TOP - USER_STACK_SIZE; page < USER_STACK_TOP; page += PAGE_SIZE) {
        paddr_t paddr = pmm_alloc_pages(1);
        if (!paddr) ke_panic("Failed to allocate user stack page");
        paging_map_page_invl(task->page_map, paddr, page, PTE_USER | PTE_WRITABLE, 0);
    }

    cpu_context_t *ctx = (cpu_context_t *)(kernel_stack_top - sizeof(cpu_context_t));
    memset(ctx, 0, sizeof(cpu_context_t));
    ctx->rip = entry;
    ctx->cs  = cs;
    ctx->ss  = ss;
    ctx->rflags = 0x202;
    ctx->rsp = (cs & 0x3) ? USER_STACK_TOP : kernel_stack_top;
    
    task->task_privilege = (cs & 0x3) ? P_USER : P_KERNEL;
    task->context = ctx;
    FP_Init(task->fx_state);

    sched_init_task_heap(task);

    task->fd_table = vfs_fd_table_clone(parent ? parent->fd_table : NULL);
    if (!task->fd_table)
        ke_panic("Failed to allocate task fd table");

    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    queue_task(task);
    spinlock_release(&sched_lock);
    irq_restore(flags);

    task->current_directory = vfs_get_root();
    task->current_directory->shared++;

    return task;
}

void itterate_each_task(task_itteration_fn fn, void *userdata) {
    if (!task_list_head || !fn) return;

    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);

    task_t *start = task_list_head;
    task_t *task = start;
    do {
        fn(task, userdata);
        task = task->next;
    } while (task != start);

    spinlock_release(&sched_lock);
    irq_restore(flags);
}

uint64_t get_task_count(void) {
    if (!task_list_head) return 0;

    uint64_t count = 0;
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);

    task_t *start = task_list_head;
    task_t *task = start;
    do {
        count++;
        task = task->next;
    } while (task != start);

    spinlock_release(&sched_lock);
    irq_restore(flags);
    return count;
}

void sched_init_task_heap(task_t* task) {
    if (!task) return;
    user_heap_t* heap = kmalloc(sizeof(user_heap_t));
    heap->task = task;
    heap->current = 0x0000004000000000ULL;
    heap->end = heap->current;
    task->heap = heap;
}
