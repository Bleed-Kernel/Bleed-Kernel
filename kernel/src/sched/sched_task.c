#include <sched/scheduler.h>
#include <panic.h>
#include <mm/kalloc.h>
#include <string.h>
#include <mm/paging.h>
#include <stdio.h>
#include <ansii.h>
#include <mm/spinlock.h>
#include <user/errno.h>

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

static void free_pid(int pid) {
    if (pid <= 0 || pid >= MAX_PIDS)
        return;

    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    pid_list[pid] = 0;
    spinlock_release(&sched_lock);
    irq_restore(flags);
}

static void free_user_alloc_list(user_alloc_t *list) {
    while (list) {
        user_alloc_t *next = list->next;
        kfree(list, sizeof(user_alloc_t));
        list = next;
    }
}

static user_alloc_t *clone_user_alloc_list(const user_alloc_t *src) {
    user_alloc_t *head = NULL;
    user_alloc_t *tail = NULL;

    while (src) {
        user_alloc_t *node = kmalloc(sizeof(user_alloc_t));
        if (!node) {
            free_user_alloc_list(head);
            return NULL;
        }

        node->vaddr = src->vaddr;
        node->pages = src->pages;
        node->next = NULL;

        if (!head)
            head = node;
        else
            tail->next = node;
        tail = node;
        src = src->next;
    }

    return head;
}

static void queue_task(task_t *task) {
    if (!task_list_head) {
        task_list_head = task;
        task->next = task;
    } else {
        task_t *tail = task_list_head;
        while (tail->next != task_list_head)
            tail = tail->next;
        tail->next = task;
        task->next = task_list_head;
    }

    if (task->state == TASK_READY)
        ready_enqueue(task);
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
    kernel_stack_top &= ~0xFULL; // this should ensure we are 16 byte aligned

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

task_t *sched_fork_from_context(cpu_context_t *parent_ctx) {
    task_t *parent = get_current_task();
    if (!parent || !parent_ctx || parent->task_privilege != P_USER)
        return NULL;

    paddr_t child_cr3 = paging_create_address_space();
    if (!child_cr3)
        return NULL;

    if (paging_clone_user_space(parent->page_map, child_cr3) != 0) {
        paging_destroy_address_space(child_cr3);
        return NULL;
    }

    task_t *child = kmalloc(sizeof(task_t));
    if (!child) {
        paging_destroy_address_space(child_cr3);
        return NULL;
    }
    memset(child, 0, sizeof(task_t));

    uint64_t pid = alloc_pid();
    if (pid == (uint64_t)-1 || pid == 0) {
        kfree(child, sizeof(task_t));
        paging_destroy_address_space(child_cr3);
        return NULL;
    }

    child->id = pid;
    child->ppid = parent->id;
    child->pgid = parent->pgid ? parent->pgid : parent->id;
    child->sid = parent->sid ? parent->sid : parent->id;
    child->state = TASK_READY;
    child->quantum_remaining = QUANTUM;
    child->page_map = child_cr3;
    child->task_privilege = parent->task_privilege;
    child->wait_target_pid = 0;
    child->wait_queue = NULL;
    child->wait_next = NULL;
    child->dead_next = NULL;
    child->exit_code = 0;
    child->exit_signal = 0;
    child->sig_pending = 0;
    child->sig_blocked = parent->sig_blocked;
    child->sig_active_frame = 0;

    memcpy(child->sig_handlers, parent->sig_handlers, sizeof(child->sig_handlers));
    memcpy(child->sig_masks, parent->sig_masks, sizeof(child->sig_masks));
    memcpy(child->sig_flags, parent->sig_flags, sizeof(child->sig_flags));
    memcpy(child->sig_restorers, parent->sig_restorers, sizeof(child->sig_restorers));

    strncpy(child->name, parent->name, sizeof(child->name) - 1);
    child->name[sizeof(child->name) - 1] = '\0';

    child->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!child->kernel_stack) {
        free_pid((int)pid);
        kfree(child, sizeof(task_t));
        paging_destroy_address_space(child_cr3);
        return NULL;
    }

    uint64_t kernel_stack_top = (uint64_t)child->kernel_stack + KERNEL_STACK_SIZE;
    child->context = (cpu_context_t *)(kernel_stack_top - sizeof(cpu_context_t));
    memcpy(child->context, parent_ctx, sizeof(cpu_context_t));
    child->context->rax = 0;
    FP_Init(child->fx_state);

    child->fd_table = vfs_fd_table_clone(parent->fd_table);
    if (!child->fd_table) {
        free_pid((int)pid);
        kfree(child->kernel_stack, KERNEL_STACK_SIZE);
        kfree(child, sizeof(task_t));
        paging_destroy_address_space(child_cr3);
        return NULL;
    }

    child->alloc_list = clone_user_alloc_list(parent->alloc_list);
    if (parent->alloc_list && !child->alloc_list) {
        free_pid((int)pid);
        vfs_fd_table_drop(child->fd_table);
        kfree(child->kernel_stack, KERNEL_STACK_SIZE);
        kfree(child, sizeof(task_t));
        paging_destroy_address_space(child_cr3);
        return NULL;
    }

    if (parent->heap) {
        child->heap = kmalloc(sizeof(user_heap_t));
        if (!child->heap) {
            free_pid((int)pid);
            free_user_alloc_list(child->alloc_list);
            vfs_fd_table_drop(child->fd_table);
            kfree(child->kernel_stack, KERNEL_STACK_SIZE);
            kfree(child, sizeof(task_t));
            paging_destroy_address_space(child_cr3);
            return NULL;
        }
        child->heap->current = parent->heap->current;
        child->heap->end = parent->heap->end;
        child->heap->task = child;
    }

    child->current_directory = parent->current_directory ? parent->current_directory : vfs_get_root();
    if (child->current_directory)
        child->current_directory->shared++;

    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    queue_task(child);
    spinlock_release(&sched_lock);
    irq_restore(flags);

    return child;
}
