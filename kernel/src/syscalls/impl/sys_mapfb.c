#include <sched/scheduler.h>
#include <stdint.h>
#include <stddef.h>
#include <drivers/framebuffer/framebuffer.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>
#include <user/errno.h>

#define USER_MMAP_BASE  0x0000004000000000ULL
#define USER_MMAP_LIMIT 0x00007fffffe00000ULL

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

    uintptr_t fb_phys_aligned = fb_phys & ~(PAGE_SIZE - 1);
    size_t offset = fb_phys & (PAGE_SIZE - 1);
    size_t fb_size = framebuffer_get_height(0) * framebuffer_get_pitch(0);
    size_t pages = (offset + fb_size + (PAGE_SIZE - 1)) / PAGE_SIZE;

    SMAP_ALLOW{ *out_pages = pages; }

    uintptr_t base = USER_MMAP_BASE;
    user_alloc_t *prev = NULL;
    user_alloc_t *next = task->alloc_list;
    for (user_alloc_t* a = task->alloc_list; a; prev = a, a = a->next) {
        if (((uintptr_t)a->vaddr - base) / PAGE_SIZE >= pages) {
            next = a;
            break;
        }
        base = (uintptr_t)a->vaddr + (a->pages * PAGE_SIZE);
        next = a->next;
    }

    if (base > USER_MMAP_LIMIT || pages > (USER_MMAP_LIMIT - base) / PAGE_SIZE)
        return (void *)(uintptr_t)-ENOMEM;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t p = fb_phys_aligned + (i * PAGE_SIZE);
        uintptr_t v = base + (i * PAGE_SIZE);

        uintptr_t clean_p = p & 0x000FFFFFFFFFF000ULL;
        
        paging_map_page_invl(task->page_map, clean_p, v, PTE_PRESENT | PTE_WRITABLE | PTE_USER, 0);
    }

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc) {
        vmm_unmap_pages(task->page_map, (void *)base, pages);
        return (void *)(uintptr_t)-ENOMEM;
    }

    alloc->vaddr = (void*)base;
    alloc->pages = pages;
    alloc->next = next;
    if (prev) prev->next = alloc;
    else task->alloc_list = alloc;

    return (void*)(base + offset);
}
