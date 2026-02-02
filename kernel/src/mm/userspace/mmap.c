#include <sched/scheduler.h>
#include <string.h>
#include <mm/kalloc.h>

void* task_mmap(task_t* task, size_t pages) {
    if (!task || !pages) return NULL;

    uintptr_t base = 0x0000004000000000ULL;
    user_alloc_t* last = NULL;

    for (user_alloc_t* a = task->alloc_list; a; last = a, a = a->next) {
        uintptr_t gap_start = base;
        uintptr_t gap_end   = (uintptr_t)a->vaddr;
        size_t gap_pages = (gap_end - gap_start) / 4096;

        if (gap_pages >= pages) break;

        base = (uintptr_t)a->vaddr + (a->pages * 4096);
    }

    if (base + pages * 4096 > 0x00007fffffe00000ULL) return NULL;

    for (size_t i = 0; i < pages; i++) {
        paddr_t phys = pmm_alloc_pages(1);
        if (!phys) return NULL;

        paging_map_page(task->page_map, phys, base + i * 4096, 0x7);
    }

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    alloc->vaddr = (void*)base;
    alloc->pages = pages;
    alloc->next = NULL;

    if (last) last->next = alloc;
    else task->alloc_list = alloc;

    return (void*)base;
}

void task_munmap(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t* prev = NULL;
    user_alloc_t* a = task->alloc_list;

    while (a) {
        if (a->vaddr == addr) {
            for (size_t i = 0; i < a->pages; i++)
                paging_map_page(task->page_map, 0, (uintptr_t)addr + i * 4096, 0);

            pmm_free_pages(vaddr_to_paddr(addr), a->pages);

            if (prev) prev->next = a->next;
            else task->alloc_list = a->next;

            kfree(a, sizeof(user_alloc_t));
            return;
        }
        prev = a;
        a = a->next;
    }
}
