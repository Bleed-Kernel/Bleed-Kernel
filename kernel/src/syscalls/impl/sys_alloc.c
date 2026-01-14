#include <stdint.h>
#include <sched/scheduler.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>

uintptr_t sys_alloc(uint64_t pages) {
    if (pages == 0) return (uintptr_t)-1;

    task_t* t = get_current_task();
    if (!t) return (uintptr_t)-1;

    static uintptr_t user_heap_base = 0x420000;
    void* va = (void*)user_heap_base;

    if (vmm_map_pages(t->page_map, va, pages, PTE_PRESENT | PTE_USER | PTE_WRITABLE) != 0)
        return (uintptr_t)-1;

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc) return (uintptr_t)-1;
    alloc->vaddr = va;
    alloc->pages = pages;
    alloc->next = t->alloc_list;
    t->alloc_list = alloc;

    user_heap_base += pages * PAGE_SIZE;
    return (uintptr_t)va;
}
