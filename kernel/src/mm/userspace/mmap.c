#include <sched/scheduler.h>
#include <string.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/paging.h>

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

/// @brief like task_mmap_reserve but at addr, for things that have to live somewhere exact
/// @return addr, NULL if any of it is already taken or outside the mmap window
void* task_mmap_reserve_at(task_t* task, uintptr_t addr, size_t pages) {
    if (!task || !pages || (addr & (PAGE_SIZE - 1)))
        return NULL;
    if (addr < USER_MMAP_BASE || addr > USER_MMAP_LIMIT)
        return NULL;
    if (pages > (USER_MMAP_LIMIT - addr) / PAGE_SIZE)
        return NULL;

    uintptr_t end = addr + pages * PAGE_SIZE;
    user_alloc_t* prev = NULL;
    user_alloc_t* next = task->alloc_list;

    for (; next; prev = next, next = next->next) {
        uintptr_t a_start = (uintptr_t)next->vaddr;
        uintptr_t a_end   = a_start + next->pages * PAGE_SIZE;

        if (a_start >= end)
            break;
        if (a_end > addr)
            return NULL;
    }

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc)
        return NULL;

    alloc->vaddr = (void*)addr;
    alloc->pages = pages;
    alloc->next = next;

    if (prev)
        prev->next = alloc;
    else
        task->alloc_list = alloc;

    return (void*)addr;
}

/// @brief forget a reservation, whatever is mapped there is left alone
void task_mmap_release(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t *a = alloc_list_unlink(task, addr);
    if (a) kfree(a);
}

#define MMAP_RUN_MAX    64

void* task_mmap(task_t* task, size_t pages) {
    uintptr_t base = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base) return NULL;

    size_t done = 0;
    size_t want = MMAP_RUN_MAX;

    while (done < pages) {
        if (want > pages - done)
            want = pages - done;

        // fall back to smaller ones when fragmented
        paddr_t phys = want > 1 ? pmm_try_alloc_pages(want) : pmm_alloc_pages(1);
        if (!phys && want > 1) {
            want /= 2;
            continue;
        }

        // zeroed, a task should never see what the last owner of the frame left behind
        if (phys)
            memset(paddr_to_vaddr(phys), 0, want * PAGE_SIZE);

        void *at = (void *)(base + done * PAGE_SIZE);
        if (!phys || paging_map_range(task->page_map, phys, (uint64_t)at, want,
                                      PTE_PRESENT | PTE_WRITABLE | PTE_USER) != 0) {
            if (phys) {
                vmm_unmap_pages(task->page_map, at, want);
                pmm_free_pages(phys, want);
            }
            if (done) (void)vmm_unmap_free_pages(task->page_map, (void *)base, done);
            task_mmap_release(task, (void *)base);
            return NULL;
        }

        done += want;
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
