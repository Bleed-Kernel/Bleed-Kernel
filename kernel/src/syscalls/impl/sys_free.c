#include <stdint.h>
#include <sched/scheduler.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kalloc.h>

uintptr_t sys_free(uint64_t addr, uint64_t pages) {
    if (pages == 0) return (uintptr_t)-1;

    task_t* t = get_current_task();
    if (!t) return (uintptr_t)-1;

    user_alloc_t** prev = &t->alloc_list;
    user_alloc_t* iter = t->alloc_list;

    while (iter) {
        if (iter->vaddr == (void*)addr) {
            vmm_unmap_pages(t->page_map, iter->vaddr, iter->pages);
            pmm_free_pages(vaddr_to_paddr(iter->vaddr), iter->pages);

            *prev = iter->next;
            kfree(iter, sizeof(user_alloc_t));
            return 0;
        }
        prev = &iter->next;
        iter = iter->next;
    }

    return (uintptr_t)-1;
}
