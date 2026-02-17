#include <sched/scheduler.h>
#include <stdint.h>
#include <stddef.h>
#include <drivers/framebuffer/framebuffer.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <user/errno.h>

void* sys_mapfb(size_t *out_pages) {
    if (!out_pages)
        return (void *)(uintptr_t)-EFAULT;

    task_t *task = get_current_task();
    if (!task)
        return (void *)(uintptr_t)-ESRCH;

    uintptr_t fb_phys = (uintptr_t)framebuffer_get_addr(0);
    
    if (fb_phys >= 0xFFFF800000000000ULL) {
        fb_phys = vaddr_to_paddr((void*)fb_phys);
    }

    uintptr_t fb_phys_aligned = fb_phys & ~0xFFFULL;
    size_t offset = fb_phys & 0xFFF;
    size_t fb_size = framebuffer_get_width(0) * framebuffer_get_height(0) * (framebuffer_get_bpp(0) / 8);
    size_t pages = (offset + fb_size + 4095) / 4096;

    SMAP_ALLOW{ *out_pages = pages; }

    uintptr_t base = 0x0000004000000000ULL;
    user_alloc_t* last = NULL;
    for (user_alloc_t* a = task->alloc_list; a; last = a, a = a->next) {
        if (((uintptr_t)a->vaddr - base) / 4096 >= pages) break;
        base = (uintptr_t)a->vaddr + (a->pages * 4096);
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t p = fb_phys_aligned + (i * 4096);
        uintptr_t v = base + (i * 4096);

        uintptr_t clean_p = p & 0x000FFFFFFFFFF000ULL;
        
        paging_map_page_invl(task->page_map, clean_p, v, PTE_PRESENT | PTE_WRITABLE | PTE_USER, 0);
    }

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (alloc) {
        alloc->vaddr = (void*)base;
        alloc->pages = pages;
        alloc->next = NULL;
        if (last) last->next = alloc;
        else task->alloc_list = alloc;
    }

    return (void*)(base + offset);
}
