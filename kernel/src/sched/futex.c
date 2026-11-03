#include <sched/futex.h>
#include <sched/scheduler.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define FUTEX_BUCKETS   64

// private ones are told apart by address space, shared memory ones by the frame
typedef struct futex_key {
    uint64_t space;
    uint64_t addr;
} futex_key_t;

typedef struct futex_waiter {
    futex_key_t         key;
    uint32_t            bitset;
    int                 woken;
    wait_queue_t        queue;
    struct futex_waiter *prev;
    struct futex_waiter *next;
} futex_waiter_t;

static spinlock_t      futex_lock;
static futex_waiter_t *buckets[FUTEX_BUCKETS];

static int futex_key(task_t *task, uint64_t uaddr, int priv, futex_key_t *key) {
    if ((uaddr & 3) || !user_ptr_valid(uaddr) || !user_ptr_valid(uaddr + 3))
        return -EINVAL;

    key->space = task->page_map & PADDR_ENTRY_MASK;
    key->addr  = uaddr;
    if (priv)
        return 0;

    uint64_t *pte = paging_get_page(task->page_map, PAGE_ALIGN_DOWN(uaddr), 0);
    if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_SHARED)) {
        key->space = 0;
        key->addr  = (*pte & PADDR_ENTRY_MASK) | (uaddr & (PAGE_SIZE - 1));
    }
    return 0;
}

static futex_waiter_t **bucket_of(const futex_key_t *key) {
    uint64_t h = (key->addr >> 2) ^ (key->space >> 12);
    h ^= h >> 17;
    return &buckets[h % FUTEX_BUCKETS];
}

static int key_eq(const futex_key_t *a, const futex_key_t *b) {
    return a->space == b->space && a->addr == b->addr;
}

static void waiter_link(futex_waiter_t *w) {
    futex_waiter_t **b = bucket_of(&w->key);
    w->prev = NULL;
    w->next = *b;
    if (*b)
        (*b)->prev = w;
    *b = w;
}

static void waiter_unlink(futex_waiter_t *w) {
    if (w->prev)
        w->prev->next = w->next;
    else
        *bucket_of(&w->key) = w->next;
    if (w->next)
        w->next->prev = w->prev;
    w->prev = w->next = NULL;
}

long futex_wait(task_t *task, uint64_t uaddr, uint32_t val, uint64_t deadline, uint32_t bitset, int priv) {
    if (!bitset)
        return -EINVAL;

    futex_waiter_t w = { .bitset = bitset };
    if (futex_key(task, uaddr, priv, &w.key) != 0)
        return -EINVAL;

    wait_entry_t entry = { .queue = &w.queue };

    unsigned long flags = irq_push();
    spinlock_acquire(&futex_lock);

    // checked under the lock, a wake cant get in between this and us being on the list
    uint32_t cur;
    if (copy_from_user(task, &cur, (const void *)uaddr, sizeof(cur)) != 0) {
        spinlock_release(&futex_lock);
        irq_restore(flags);
        return -EFAULT;
    }
    if (cur != val) {
        spinlock_release(&futex_lock);
        irq_restore(flags);
        return -EAGAIN;
    }

    waiter_link(&w);
    sched_prepare_wait(&entry, 1, deadline);
    spinlock_release(&futex_lock);

    // a signal sent while it was still in user mode wont wake it, it has to look itself
    if (!(task->sig_pending & ~task->sig_blocked))
        sched_wait_commit();
    sched_finish_wait();

    spinlock_acquire(&futex_lock);
    if (!w.woken)
        waiter_unlink(&w);
    spinlock_release(&futex_lock);
    irq_restore(flags);

    if (w.woken)
        return 0;
    if (deadline && sched_deadline_passed(deadline))
        return -ETIMEDOUT;
    return -EINTR;
}

// futex_lock held
static int wake_key(const futex_key_t *key, int count, uint32_t bitset) {
    int woken = 0;
    futex_waiter_t *w = *bucket_of(key);

    while (w && woken < count) {
        futex_waiter_t *next = w->next;
        if (key_eq(&w->key, key) && (w->bitset & bitset)) {
            waiter_unlink(w);
            w->woken = 1;
            sched_wake_queue(&w->queue);
            woken++;
        }
        w = next;
    }
    return woken;
}

long futex_wake(task_t *task, uint64_t uaddr, int count, uint32_t bitset, int priv) {
    if (!bitset)
        return -EINVAL;
    if (count <= 0)
        return 0;

    futex_key_t key;
    if (futex_key(task, uaddr, priv, &key) != 0)
        return -EINVAL;

    unsigned long flags = irq_push();
    spinlock_acquire(&futex_lock);
    int woken = wake_key(&key, count, bitset);
    spinlock_release(&futex_lock);
    irq_restore(flags);
    return woken;
}

long futex_requeue(task_t *task, uint64_t uaddr, uint64_t uaddr2, int count, int requeue,
                   int cmp, uint32_t val3, int priv) {
    if (count < 0 || requeue < 0)
        return -EINVAL;

    futex_key_t key, key2;
    if (futex_key(task, uaddr, priv, &key) != 0 || futex_key(task, uaddr2, priv, &key2) != 0)
        return -EINVAL;

    unsigned long flags = irq_push();
    spinlock_acquire(&futex_lock);

    if (cmp) {
        uint32_t cur;
        long err = 0;
        if (copy_from_user(task, &cur, (const void *)uaddr, sizeof(cur)) != 0)
            err = -EFAULT;
        else if (cur != val3)
            err = -EAGAIN;

        if (err) {
            spinlock_release(&futex_lock);
            irq_restore(flags);
            return err;
        }
    }

    int done = wake_key(&key, count, FUTEX_BITSET_MATCH_ANY);

    // whoever is left moves over still asleep, to be woken off uaddr2 later
    futex_waiter_t *w = *bucket_of(&key);
    int moved = 0;
    while (w && moved < requeue) {
        futex_waiter_t *next = w->next;
        if (key_eq(&w->key, &key)) {
            waiter_unlink(w);
            w->key = key2;
            waiter_link(w);
            moved++;
        }
        w = next;
    }

    spinlock_release(&futex_lock);
    irq_restore(flags);
    return done + moved;
}
