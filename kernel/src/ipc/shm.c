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

typedef struct shm_object {
    uint64_t id;
    uint64_t owner_pid;
    size_t   pages;
    paddr_t *frames;
    struct shm_object *next;
} shm_object_t;

static shm_object_t *shm_objects;
static uint64_t      shm_next_id = 1;
static spinlock_t    shm_lock = {0};

static shm_object_t *shm_find(uint64_t id) {
    if (!id) return NULL;

    for (shm_object_t *obj = shm_objects; obj; obj = obj->next) {
        if (obj->id == id)
            return obj;
    }
    return NULL;
}

// off the list, whoever called owns it and its frames after this
static void shm_unlink(shm_object_t *obj) {
    for (shm_object_t **p = &shm_objects; *p; p = &(*p)->next) {
        if (*p == obj) {
            *p = obj->next;
            return;
        }
    }
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

    shm_object_t *obj    = kmalloc(sizeof(shm_object_t));
    paddr_t      *frames = kmalloc((size_t)pages * sizeof(paddr_t));
    if (!obj || !frames) {
        if (obj)    kfree(obj);
        if (frames) kfree(frames);
        return -ENOMEM;
    }

    for (size_t i = 0; i < (size_t)pages; i++) {
        // zeroed, same reason as task_mmap
        paddr_t phys = paging_alloc_empty_frame(NULL);
        if (!phys) {
            shm_drop_frames(frames, i);
            kfree(obj);
            return -ENOMEM;
        }

        // no refcount table means nothing would stop the frame being freed while mapped
        cow_ref_page(phys);
        if (!cow_get_refcount(phys)) {
            pmm_free_pages(phys, 1);
            shm_drop_frames(frames, i);
            kfree(obj);
            return -ENOMEM;
        }
        frames[i] = phys;
    }

    unsigned long flags = irq_push();
    spinlock_acquire(&shm_lock);

    uint64_t id    = shm_next_id++;
    obj->id        = id;
    obj->owner_pid = task->id;
    obj->pages     = (size_t)pages;
    obj->frames    = frames;
    obj->next      = shm_objects;
    shm_objects    = obj;

    spinlock_release(&shm_lock);
    irq_restore(flags);

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

long shm_map_at(task_t *task, uint64_t id, uint64_t first, uint64_t pages, uint64_t addr) {
    if (!task)
        return -ESRCH;
    if (!pages)
        return -EINVAL;

    unsigned long flags = irq_push();
    spinlock_acquire(&shm_lock);

    shm_object_t *obj = shm_find(id);
    if (!obj || first > obj->pages || pages > obj->pages - first) {
        spinlock_release(&shm_lock);
        irq_restore(flags);
        return obj ? -EINVAL : -ENOENT;
    }

    if (!task_mmap_reserve_at(task, (uintptr_t)addr, (size_t)pages)) {
        spinlock_release(&shm_lock);
        irq_restore(flags);
        return -EEXIST;
    }

    // left executable, the point is running code out of it
    for (size_t i = 0; i < (size_t)pages; i++) {
        cow_ref_page(obj->frames[first + i]);
        paging_map_page_invl(task->page_map, obj->frames[first + i], addr + i * PAGE_SIZE,
                             PTE_USER | PTE_WRITABLE | PTE_SHARED, 1);
    }

    spinlock_release(&shm_lock);
    irq_restore(flags);
    return (long)addr;
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

    shm_unlink(obj);

    spinlock_release(&shm_lock);
    irq_restore(flags);

    // freed with the lock gone, nothing can find it any more
    shm_drop_frames(obj->frames, obj->pages);
    kfree(obj);
    return 0;
}

void shm_task_cleanup(task_t *task) {
    if (!task)
        return;

    for (;;) {
        unsigned long flags = irq_push();
        spinlock_acquire(&shm_lock);

        shm_object_t *obj = shm_objects;
        while (obj && obj->owner_pid != task->id)
            obj = obj->next;
        if (obj)
            shm_unlink(obj);

        spinlock_release(&shm_lock);
        irq_restore(flags);

        if (!obj)
            break;
        shm_drop_frames(obj->frames, obj->pages);
        kfree(obj);
    }
}
