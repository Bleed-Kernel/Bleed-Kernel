#include <sched/scheduler.h>
#include <string.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>

#define USER_MMAP_BASE  0x0000004000000000ULL
#define USER_MMAP_LIMIT 0x00007fffffe00000ULL

static void rollback_mapping(task_t *task, uintptr_t base, size_t mapped_pages) {
    if (!mapped_pages)
        return;
    (void)vmm_unmap_free_pages(task->page_map, (void *)base, mapped_pages);
}

void* task_mmap(task_t* task, size_t pages) {
    if (!task || !pages) return NULL;

    uintptr_t base = USER_MMAP_BASE;
    user_alloc_t* prev = NULL;
    user_alloc_t* next = task->alloc_list;

    for (user_alloc_t* a = task->alloc_list; a; prev = a, a = a->next) {
        uintptr_t gap_start = base;
        uintptr_t gap_end   = (uintptr_t)a->vaddr;
        size_t gap_pages = (gap_end - gap_start) / PAGE_SIZE;

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

    size_t mapped_pages = 0;
    for (size_t i = 0; i < pages; i++) {
        paddr_t phys = pmm_alloc_pages(1);
        if (!phys) {
            rollback_mapping(task, base, mapped_pages);
            return NULL;
        }

        paging_map_page_invl(task->page_map, phys, base + i * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE | PTE_USER , 0);
        mapped_pages++;
    }

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc) {
        rollback_mapping(task, base, mapped_pages);
        return NULL;
    }

    alloc->vaddr = (void*)base;
    alloc->pages = pages;
    alloc->next = next;

    if (prev)
        prev->next = alloc;
    else
        task->alloc_list = alloc;

    return (void*)base;
}

void task_munmap(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t* prev = NULL;
    user_alloc_t* a = task->alloc_list;

    while (a) {
        if (a->vaddr == addr) {
            (void)vmm_unmap_free_pages(task->page_map, addr, a->pages);

            if (prev) prev->next = a->next;
            else task->alloc_list = a->next;

            kfree(a, sizeof(user_alloc_t));
            return;
        }
        prev = a;
        a = a->next;
    }
}
