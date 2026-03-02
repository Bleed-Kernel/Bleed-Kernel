#include <mm/cow.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <string.h>

static uint32_t *cow_refs;
static size_t cow_ref_count;
static spinlock_t cow_lock;
static int cow_ready;
static int cow_lock_init_done;

static inline size_t cow_phys_to_index(paddr_t phys) {
    return (size_t)((phys & ~(PAGE_SIZE - 1)) / PAGE_SIZE);
}

void cow_init(void) {
    if (cow_ready)
        return;

    if (!cow_lock_init_done) {
        spinlock_init(&cow_lock);
        cow_lock_init_done = 1;
    }

    size_t frame_count = (get_max_paddr() + (PAGE_SIZE - 1)) / PAGE_SIZE;
    size_t bytes = frame_count * sizeof(uint32_t);
    uint32_t *table = kmalloc(bytes);
    if (!table)
        return;

    memset(table, 0, bytes);

    unsigned long flags = irq_push();
    spinlock_acquire(&cow_lock);
    if (!cow_ready) {
        cow_refs = table;
        cow_ref_count = frame_count;
        cow_ready = 1;
        table = NULL;
    }
    spinlock_release(&cow_lock);
    irq_restore(flags);

    if (table)
        kfree(table, bytes);
}

void cow_ref_page(paddr_t phys) {
    cow_init();
    if (!cow_ready)
        return;

    size_t idx = cow_phys_to_index(phys);

    unsigned long flags = irq_push();
    spinlock_acquire(&cow_lock);
    if (idx < cow_ref_count)
        cow_refs[idx]++;
    spinlock_release(&cow_lock);
    irq_restore(flags);
}

uint32_t cow_unref_page(paddr_t phys) {
    cow_init();
    if (!cow_ready)
        return 0;

    size_t idx = cow_phys_to_index(phys);
    uint32_t refs = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&cow_lock);
    if (idx < cow_ref_count && cow_refs[idx] > 0) {
        cow_refs[idx]--;
        refs = cow_refs[idx];
    }
    spinlock_release(&cow_lock);
    irq_restore(flags);

    return refs;
}

uint32_t cow_get_refcount(paddr_t phys) {
    cow_init();
    if (!cow_ready)
        return 0;

    size_t idx = cow_phys_to_index(phys);
    uint32_t refs = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&cow_lock);
    if (idx < cow_ref_count)
        refs = cow_refs[idx];
    spinlock_release(&cow_lock);
    irq_restore(flags);

    return refs;
}
