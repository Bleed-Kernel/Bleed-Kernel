#include <sched/scheduler.h>
#include <stdint.h>
#include <stddef.h>
#include <drivers/framebuffer/framebuffer.h>
#include <mm/paging.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>
#include <user/errno.h>
#include <user/user_copy.h>

void* sys_mapfb(size_t *out_pages) {
    if (!out_pages)
        return (void *)(uintptr_t)-EFAULT;

    task_t *task = get_current_task();
    if (!task)
        return (void *)(uintptr_t)-ESRCH;

    uintptr_t fb_phys = (uintptr_t)g_gbi.framebuffer.phys_address;
    if (!fb_phys)
        return (void *)(uintptr_t)-ENODEV;

    uintptr_t fb_phys_aligned = fb_phys & ~(PAGE_SIZE - 1);
    size_t offset = fb_phys & (PAGE_SIZE - 1);
    size_t fb_size = framebuffer_get_height(0) * framebuffer_get_pitch(0);
    size_t pages = (offset + fb_size + (PAGE_SIZE - 1)) / PAGE_SIZE;

    if (copy_to_user(task, out_pages, &pages, sizeof(pages)) != 0)
        return (void *)(uintptr_t)-EFAULT;

    uintptr_t base = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base)
        return (void *)(uintptr_t)-ENOMEM;

    // NOFREE so unmap, fork and exit all know these frames are the device and not ram
    for (size_t i = 0; i < pages; i++) {
        uintptr_t p = fb_phys_aligned + (i * PAGE_SIZE);
        uintptr_t v = base + (i * PAGE_SIZE);

        paging_map_page_wc(task->page_map, p, v, PTE_WRITABLE | PTE_USER | PTE_NX | PTE_NOFREE);
    }

    return (void*)(base + offset);
}
