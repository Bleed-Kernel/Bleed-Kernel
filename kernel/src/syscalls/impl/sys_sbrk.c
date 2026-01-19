#include <stdint.h>
#include <sched/scheduler.h>
#include <mm/vmm.h>
#include <mm/paging.h>
#include <mm/heap.h>

uintptr_t sys_sbrk(intptr_t increment) {
    task_t* t = get_current_task();
    if (!t)
        return (uintptr_t)-1;

    uintptr_t old_end = t->heap_end;
    uintptr_t new_end = old_end + increment;

    if (new_end < t->heap_base)
        return (uintptr_t)-1;

    if (increment > 0) {
        uintptr_t map_start = ALIGN_UP(old_end, PAGE_SIZE);
        uintptr_t map_end   = ALIGN_UP(new_end, PAGE_SIZE);

        if (map_end > map_start) {
            size_t pages = (map_end - map_start) / PAGE_SIZE;
            if (vmm_map_pages(
                    t->page_map,
                    (void*)map_start,
                    pages,
                    PTE_PRESENT | PTE_USER | PTE_WRITABLE
                ) != 0)
                return (uintptr_t)-1;
        }
    }

    if (increment < 0) {
        uintptr_t unmap_start = ALIGN_UP(new_end, PAGE_SIZE);
        uintptr_t unmap_end   = ALIGN_UP(old_end, PAGE_SIZE);

        if (unmap_end > unmap_start) {
            size_t pages = (unmap_end - unmap_start) / PAGE_SIZE;
            vmm_unmap_pages(
                t->page_map,
                (void*)unmap_start,
                pages
            );
        }
    }

    t->heap_end = new_end;
    return old_end;
}
