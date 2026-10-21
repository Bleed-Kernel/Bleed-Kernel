#include <sched/scheduler.h>
#include <kernel/exception/panic.h>
#include <mm/kalloc.h>
#include <string.h>
#include <mm/paging.h>
#include <stdio.h>
#include <ansii.h>
#include <mm/spinlock.h>
#include <user/errno.h>
#include <cpu/features/fpu.h>

#include "priv_scheduler.h"

#define PID_HASH_SIZE   256

spinlock_t sched_lock;

static uint64_t pid_bitmap[MAX_PIDS / 64];
static int      pid_cursor = 1;
static task_t  *pid_hash[PID_HASH_SIZE];
static uint64_t task_count = 0;

__attribute__((constructor))
void sched_init_lock(void) {
    spinlock_init(&sched_lock);
}

// first free pid at or after from, -1 if theres none up there
static int pid_find_free(int from) {
    for (int i = from; i < MAX_PIDS; i = (i & ~63) + 64) {
        uint64_t free = ~pid_bitmap[i / 64] & (~0ULL << (i % 64));
        if (free)
            return (i & ~63) + __builtin_ctzll(free);
    }
    return -1;
}

int alloc_pid(){
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);

    int pid = pid_find_free(pid_cursor);
    if (pid < 0)
        pid = pid_find_free(1);
    if (pid > 0) {
        pid_bitmap[pid / 64] |= 1ULL << (pid % 64);
        pid_cursor = pid + 1 < MAX_PIDS ? pid + 1 : 1;
    }

    spinlock_release(&sched_lock);
    irq_restore(flags);
    return pid;
}

static void free_pid_locked(uint64_t pid) {
    if (pid == 0 || pid >= MAX_PIDS)
        return;
    pid_bitmap[pid / 64] &= ~(1ULL << (pid % 64));
}

static void free_pid(int pid) {
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    free_pid_locked((uint64_t)pid);
    spinlock_release(&sched_lock);
    irq_restore(flags);
}

task_t *sched_lookup_locked(uint64_t pid) {
    for (task_t *t = pid_hash[pid % PID_HASH_SIZE]; t; t = t->pid_next) {
        if (t->id == pid)
            return t;
    }
    return NULL;
}

static void child_link(task_t *parent, task_t *child) {
    child->parent       = parent;
    child->sibling_prev = NULL;
    child->sibling_next = parent ? parent->children : NULL;
    if (!parent)
        return;

    if (parent->children)
        parent->children->sibling_prev = child;
    parent->children = child;
}

static void child_unlink(task_t *child) {
    task_t *parent = child->parent;
    if (!parent)
        return;

    if (child->sibling_prev)
        child->sibling_prev->sibling_next = child->sibling_next;
    else
        parent->children = child->sibling_next;

    if (child->sibling_next)
        child->sibling_next->sibling_prev = child->sibling_prev;

    child->parent       = NULL;
    child->sibling_prev = NULL;
    child->sibling_next = NULL;
}

// frees the tracking nodes only, the pages themselves go with the address space
void sched_free_alloc_list(user_alloc_t *list) {
    while (list) {
        user_alloc_t *next = list->next;
        kfree(list);
        list = next;
    }
}

static user_alloc_t *clone_user_alloc_list(const user_alloc_t *src) {
    user_alloc_t *head = NULL;
    user_alloc_t *tail = NULL;

    while (src) {
        user_alloc_t *node = kmalloc(sizeof(user_alloc_t));
        if (!node) {
            sched_free_alloc_list(head);
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

void sched_queue_task_locked(task_t *task) {
    if (!task_list_head) {
        task_list_head = task;
        task->next     = task;
        task->prev     = task;
    } else {
        task_t *tail = task_list_head->prev;
        tail->next           = task;
        task->prev           = tail;
        task->next           = task_list_head;
        task_list_head->prev = task;
    }

    task_t **bucket = &pid_hash[task->id % PID_HASH_SIZE];
    task->pid_next  = *bucket;
    *bucket         = task;

    if (task->id != task->ppid)
        child_link(sched_lookup_locked(task->ppid), task);

    task_count++;

    if (task->state == TASK_READY)
        ready_enqueue(task);
}

// the reaper is done with it, after this the pid can go to someone else
void sched_unlink_task_locked(task_t *task) {
    if (task->next == task) {
        task_list_head = NULL;
    } else {
        task->prev->next = task->next;
        task->next->prev = task->prev;
        if (task_list_head == task)
            task_list_head = task->next;
    }
    task->next = task->prev = NULL;

    for (task_t **p = &pid_hash[task->id % PID_HASH_SIZE]; *p; p = &(*p)->pid_next) {
        if (*p == task) {
            *p = task->pid_next;
            break;
        }
    }

    child_unlink(task);

    // exit() hands them on already, this is for one that never got there
    task_t *reaper = sched_lookup_locked(REAPER_PID);
    while (task->children) {
        task_t *child = task->children;
        child_unlink(child);
        child->ppid = REAPER_PID;
        child_link(reaper != task ? reaper : NULL, child);
    }

    ready_dequeue(task);
    task_count--;

    // the reaper and the kernel task keep theirs
    if (task->id > 1)
        free_pid_locked(task->id);
}

task_t *sched_get_task(uint64_t pid) {
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    task_t *t = sched_lookup_locked(pid);
    spinlock_release(&sched_lock);
    irq_restore(flags);
    return t;
}

/// @brief hand the children of a dying task to the reaper, the way linux gives orphans to init
/// @param parent the task thats going away
/// @return a child that had already exited and can be buried now, NULL once there are none left
task_t *sched_reparent_children(task_t *parent) {
    if (!parent) return NULL;

    task_t *zombie = NULL;
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);

    task_t *reaper = sched_lookup_locked(REAPER_PID);
    if (reaper == parent)
        reaper = NULL;

    while (parent->children) {
        task_t *t = parent->children;
        child_unlink(t);
        t->ppid = REAPER_PID;
        child_link(reaper, t);

        // nobody is left to wait for it. handed back one at a time because
        // sched_mark_task_dead takes the lock we are holding
        if (t->state == TASK_ZOMBIE) {
            zombie = t;
            break;
        }
    }

    spinlock_release(&sched_lock);
    irq_restore(flags);
    return zombie;
}

task_t *sched_create_task(uint64_t cr3, uint64_t entry, uint64_t cs, uint64_t ss, char *task_name) {
    task_t *task = kmalloc(sizeof(task_t));
    if (!task) ke_panic(NULL, "Failed to allocate task");
    memset(task, 0, sizeof(task_t));

    int pid = alloc_pid();
    if (pid <= 0) ke_panic(NULL, "Failed to allocate PID");
    task->id = (uint64_t)pid;
    
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
    task->prio_base = parent ? parent->prio_base : PRIO_DEFAULT;
    task->page_map = cr3;

    task->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack)
        ke_panic(NULL, "Failed to allocate kernel stack");
    uint64_t kernel_stack_top = (uint64_t)task->kernel_stack + KERNEL_STACK_SIZE;
    kernel_stack_top &= ~0xFULL; // this should ensure we are 16 byte aligned

    cpu_context_t *ctx = (cpu_context_t *)(kernel_stack_top - sizeof(cpu_context_t));
    memset(ctx, 0, sizeof(cpu_context_t));
    ctx->rip = entry;
    ctx->cs  = cs;
    ctx->ss  = ss;
    ctx->rflags = 0x202;
    ctx->rsp = (cs & 0x3) ? USER_STACK_TOP : kernel_stack_top;
    
    task->task_privilege = (cs & 0x3) ? P_USER : P_KERNEL;
    task->context = ctx;
    if (fpu_task_init(task) != 0)
        ke_panic(NULL, "Failed to allocate task FPU state");

    sched_init_task_heap(task);

    task->fd_table = vfs_fd_table_clone(parent ? parent->fd_table : NULL);
    if (!task->fd_table)
        ke_panic(NULL, "Failed to allocate task fd table");

    task->current_directory = vfs_get_root();
    if (task->current_directory)
        task->current_directory->shared++;

    // queue it last, the moment its on the list a tick can run it
    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    sched_queue_task_locked(task);
    spinlock_release(&sched_lock);
    irq_restore(flags);

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
    return task_count;
}

void sched_init_task_heap(task_t* task) {
    if (!task) return;
    user_heap_t* heap = kmalloc(sizeof(user_heap_t));
    if (!heap) return;
    heap->task = task;
    heap->current = USER_MMAP_BASE;
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
        kfree(child);
        paging_destroy_address_space(child_cr3);
        return NULL;
    }

    child->id = pid;
    child->ppid = parent->id;
    child->pgid = parent->pgid ? parent->pgid : parent->id;
    child->sid = parent->sid ? parent->sid : parent->id;
    child->state = TASK_READY;
    child->quantum_remaining = QUANTUM;
    child->prio_base = parent->prio_base;
    child->prio_bonus = parent->prio_bonus;
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
    memcpy(child->exec_path, parent->exec_path, sizeof(child->exec_path));

    child->kernel_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!child->kernel_stack)
        goto fail;

    uint64_t kernel_stack_top = (uint64_t)child->kernel_stack + KERNEL_STACK_SIZE;
    kernel_stack_top &= ~0xFULL; // has to match what sched_switch_task puts in rsp0
    child->context = (cpu_context_t *)(kernel_stack_top - sizeof(cpu_context_t));
    memcpy(child->context, parent_ctx, sizeof(cpu_context_t));
    child->context->rax = 0;

    // fork is meant to carry the fpu state over, not hand the child a blank one
    if (fpu_task_init(child) != 0)
        goto fail;
    fpu_task_copy(child, parent);

    child->fd_table = vfs_fd_table_clone(parent->fd_table);
    if (!child->fd_table)
        goto fail;

    child->alloc_list = clone_user_alloc_list(parent->alloc_list);
    if (parent->alloc_list && !child->alloc_list)
        goto fail;

    if (parent->heap) {
        child->heap = kmalloc(sizeof(user_heap_t));
        if (!child->heap)
            goto fail;

        child->heap->current = parent->heap->current;
        child->heap->end = parent->heap->end;
        child->heap->task = child;
    }

    child->current_directory = parent->current_directory ? parent->current_directory : vfs_get_root();
    if (child->current_directory)
        child->current_directory->shared++;

    unsigned long flags = irq_push();
    spinlock_acquire(&sched_lock);
    sched_queue_task_locked(child);
    spinlock_release(&sched_lock);
    irq_restore(flags);

    return child;

fail:
    // child was zeroed so whatever we didnt get to is still NULL
    free_pid((int)pid);
    sched_free_alloc_list(child->alloc_list);
    vfs_fd_table_drop(child->fd_table);
    fpu_task_free(child);
    kfree(child->kernel_stack);
    kfree(child);
    paging_destroy_address_space(child_cr3);
    return NULL;
}
