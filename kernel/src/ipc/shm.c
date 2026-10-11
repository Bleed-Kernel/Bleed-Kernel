#include <ipc/shm.h>

#include <mm/cow.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/spinlock.h>
#include <mm/userspace/mmap.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <user/user_copy.h>

typedef struct {
    uint64_t id;            // 0 = slot is free
    uint64_t owner_pid;
    size_t   pages;
    paddr_t *frames;
} shm_object_t;

static shm_object_t shm_objects[SHM_MAX_OBJECTS];
static uint64_t     shm_next_id = 1;
static spinlock_t   shm_lock = {0};

static shm_object_t *shm_find(uint64_t id) {
    if (!id) return NULL;

    for (size_t i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (shm_objects[i].id == id)
            return &shm_objects[i];
    }
    return NULL;
}

// drop the objects ref on each frame and free the ones nobody has mapped
static void shm_drop_frames(paddr_t *frames, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (cow_unref_page(frames[i]) == 0)
            pmm_free_pages(frames[i], 1);
    }
    kfree(frames);
}

long shm_create(task_t *task, uint64_t pages) {
    if (!task)
        return -ESRCH;
    if (!pages || pages > SHM_MAX_PAGES)
        return -EINVAL;

    paddr_t *frames = kmalloc((size_t)pages * sizeof(paddr_t));
    if (!frames)
        return -ENOMEM;

    for (size_t i = 0; i < (size_t)pages; i++) {
        // zeroed, same reason as task_mmap
        paddr_t phys = paging_alloc_empty_frame(NULL);
        if (!phys) {
            shm_drop_frames(frames, i);
            return -ENOMEM;
        }

        // no refcount table means nothing would stop the frame being freed while mapped
        cow_ref_page(phys);
        if (!cow_get_refcount(phys)) {
            pmm_free_pages(phys, 1);
            shm_drop_frames(frames, i);
            return -ENOMEM;
        }
        frames[i] = phys;
    }

    unsigned long flags = irq_push();
    spinlock_acquire(&shm_lock);

    shm_object_t *obj = NULL;
    for (size_t i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!shm_objects[i].id) {
            obj = &shm_objects[i];
            break;
        }
    }

    uint64_t id = 0;
    if (obj) {
        id             = shm_next_id++;
        obj->id        = id;
        obj->owner_pid = task->id;
        obj->pages     = (size_t)pages;
        obj->frames    = frames;
    }

    spinlock_release(&shm_lock);
    irq_restore(flags);

    if (!id) {
        shm_drop_frames(frames, (size_t)pages);
        return -ENOSPC;
    }
    return (long)id;
}

long shm_map(task_t *task, uint64_t id, uint64_t user_pages_ptr) {
    if (!task)
        return -ESRCH;
    if (user_pages_ptr && !user_ptr_valid(user_pages_ptr))
        return -EFAULT;

    // mapped with the lock held, the owner destroying it halfway would pull the frames from under us
    unsigned long flags = irq_push();
    spinlock_acquire(&shm_lock);

    shm_object_t *obj = shm_find(id);
    if (!obj) {
        spinlock_release(&shm_lock);
        irq_restore(flags);
        return -ENOENT;
    }

    size_t    pages = obj->pages;
    uintptr_t base  = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base) {
        spinlock_release(&shm_lock);
        irq_restore(flags);
        return -ENOMEM;
    }

    for (size_t i = 0; i < pages; i++) {
        cow_ref_page(obj->frames[i]);
        paging_map_page_invl(task->page_map, obj->frames[i], base + i * PAGE_SIZE,
                             PTE_USER | PTE_WRITABLE | PTE_NX | PTE_SHARED, 0);
    }

    spinlock_release(&shm_lock);
    irq_restore(flags);

    if (user_pages_ptr && copy_to_user(task, (void *)user_pages_ptr, &pages, sizeof(pages)) != 0) {
        task_munmap(task, (void *)base);
        return -EFAULT;
    }

    return (long)base;
}

// take the frames out of the slot so they can be dropped once the lock is gone
static paddr_t *shm_slot_clear(shm_object_t *obj, size_t *pages_out) {
    paddr_t *frames = obj->frames;
    *pages_out = obj->pages;

    obj->id        = 0;
    obj->owner_pid = 0;
    obj->pages     = 0;
    obj->frames    = NULL;
    return frames;
}

long shm_destroy(task_t *task, uint64_t id) {
    if (!task)
        return -ESRCH;

    unsigned long flags = irq_push();
    spinlock_acquire(&shm_lock);

    shm_object_t *obj = shm_find(id);
    if (!obj || obj->owner_pid != task->id) {
        spinlock_release(&shm_lock);
        irq_restore(flags);
        return obj ? -EPERM : -ENOENT;
    }

    size_t   pages  = 0;
    paddr_t *frames = shm_slot_clear(obj, &pages);

    spinlock_release(&shm_lock);
    irq_restore(flags);

    shm_drop_frames(frames, pages);
    return 0;
}

void shm_task_cleanup(task_t *task) {
    if (!task)
        return;

    for (size_t i = 0; i < SHM_MAX_OBJECTS; i++) {
        unsigned long flags = irq_push();
        spinlock_acquire(&shm_lock);

        size_t   pages  = 0;
        paddr_t *frames = NULL;
        if (shm_objects[i].id && shm_objects[i].owner_pid == task->id)
            frames = shm_slot_clear(&shm_objects[i], &pages);

        spinlock_release(&shm_lock);
        irq_restore(flags);

        if (frames)
            shm_drop_frames(frames, pages);
    }
}
