#include <sched/scheduler.h>
#include <string.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>

// take the node tracking addr out of the tasks alloc list, the caller owns it after this
static user_alloc_t *alloc_list_unlink(task_t *task, void *addr) {
    user_alloc_t *prev = NULL;

    for (user_alloc_t *a = task->alloc_list; a; prev = a, a = a->next) {
        if (a->vaddr != addr)
            continue;

        if (prev) prev->next = a->next;
        else task->alloc_list = a->next;
        return a;
    }
    return NULL;
}

/// @brief find a free run in the mmap window and track it in the alloc list, nothing is mapped yet
/// @return base of the reserved range, NULL if it doesnt fit
void* task_mmap_reserve(task_t* task, size_t pages) {
    if (!task || !pages) return NULL;

    uintptr_t base = USER_MMAP_BASE;
    user_alloc_t* prev = NULL;
    user_alloc_t* next = task->alloc_list;

    // the list is kept sorted so the first gap that fits is the lowest one
    for (user_alloc_t* a = task->alloc_list; a; prev = a, a = a->next) {
        size_t gap_pages = ((uintptr_t)a->vaddr - base) / PAGE_SIZE;

        if (gap_pages >= pages) {
            next = a;
            break;
        }

        base = (uintptr_t)a->vaddr + (a->pages * PAGE_SIZE);
        next = a->next;
    }

    if (base > USER_MMAP_LIMIT)
        return NULL;
    if (pages > (USER_MMAP_LIMIT - base) / PAGE_SIZE)
        return NULL;

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc)
        return NULL;

    alloc->vaddr = (void*)base;
    alloc->pages = pages;
    alloc->next = next;

    if (prev)
        prev->next = alloc;
    else
        task->alloc_list = alloc;

    return (void*)base;
}

/// @brief forget a reservation, whatever is mapped there is left alone
void task_mmap_release(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t *a = alloc_list_unlink(task, addr);
    if (a) kfree(a);
}

void* task_mmap(task_t* task, size_t pages) {
    uintptr_t base = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base) return NULL;

    for (size_t i = 0; i < pages; i++) {
        // zeroed, a task should never see what the last owner of the frame left behind
        paddr_t phys = paging_alloc_empty_frame(NULL);
        if (!phys) {
            if (i) (void)vmm_unmap_free_pages(task->page_map, (void *)base, i);
            task_mmap_release(task, (void *)base);
            return NULL;
        }

        paging_map_page_invl(task->page_map, phys, base + i * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE | PTE_USER , 0);
    }

    return (void*)base;
}

void task_munmap(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t *a = alloc_list_unlink(task, addr);
    if (!a) return;

    (void)vmm_unmap_free_pages(task->page_map, addr, a->pages);
    kfree(a);
}
