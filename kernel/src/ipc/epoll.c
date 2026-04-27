#include <ipc/epoll.h>

#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <ACPI/acpi_hpet.h>

static inline uint64_t epoll_ms_now(void) {
    uint64_t mpt = getMillisecondsPerTick();
    if (!mpt) return 0;
    return *(volatile uint64_t *)((uint8_t *)getHpetAddress() + 0xF0) / mpt;
}

void poll_table_init(poll_table_t *pt) {
    spinlock_init(&pt->lock);
    pt->head = NULL;
}

void poll_notify(poll_table_t *pt, uint32_t events) {
    if (!pt) return;

    unsigned long irq = irq_push();
    spinlock_acquire(&pt->lock);
    poll_wakeup_t *wake = pt->head;
    spinlock_release(&pt->lock);
    irq_restore(irq);

    while (wake) {
        epoll_instance_t *ep = wake->instance;
        if (!ep) {
            wake = wake->next;
            continue;
        }

        irq = irq_push();
        spinlock_acquire(&ep->lock);

        epoll_entry_t *entry = ep->entries;
        while (entry && entry->fd != wake->fd)
            entry = entry->next;

        if (entry && (entry->events & events)) {
            if (!entry->ready) {
                entry->ready      = 1;
                entry->ready_next = NULL;

                if (!ep->ready_tail) {
                    ep->ready_head = entry;
                    ep->ready_tail = entry;
                } else {
                    ep->ready_tail->ready_next = entry;
                    ep->ready_tail             = entry;
                }
                ep->ready_count++;

                if (ep->waiter_task) {
                    task_t *t       = ep->waiter_task;
                    ep->waiter_task = NULL;
                    sched_wake(t);
                }
            }
        }

        spinlock_release(&ep->lock);
        irq_restore(irq);

        wake = wake->next;
    }
}

// wakeup register
static int poll_table_add_wakeup(poll_table_t *pt, epoll_instance_t *ep, int fd) {
    poll_wakeup_t *wake = kmalloc(sizeof(poll_wakeup_t));
    if (!wake)
        return -ENOMEM;

    wake->instance = ep;
    wake->fd       = fd;

    unsigned long irq = irq_push();
    spinlock_acquire(&pt->lock);
    wake->next   = pt->head;
    pt->head     = wake;
    spinlock_release(&pt->lock);
    irq_restore(irq);

    return 0;
}

static void poll_table_remove_wakeup(poll_table_t *pt, epoll_instance_t *ep, int fd) {
    unsigned long irq = irq_push();
    spinlock_acquire(&pt->lock);

    poll_wakeup_t *prev = NULL;
    poll_wakeup_t *cur  = pt->head;
    while (cur) {
        if (cur->instance == ep && cur->fd == fd) {
            if (prev)
                prev->next = cur->next;
            else
                pt->head = cur->next;
            spinlock_release(&pt->lock);
            irq_restore(irq);
            kfree(cur, sizeof(poll_wakeup_t));
            return;
        }
        prev = cur;
        cur  = cur->next;
    }

    spinlock_release(&pt->lock);
    irq_restore(irq);
}

// epoll lifecycles
epoll_instance_t *epoll_instance_create(void) {
    epoll_instance_t *ep = kmalloc(sizeof(epoll_instance_t));
    if (!ep)
        return NULL;

    spinlock_init(&ep->lock);
    ep->entries     = NULL;
    ep->entry_count = 0;
    ep->ready_head  = NULL;
    ep->ready_tail  = NULL;
    ep->ready_count = 0;
    ep->waiter_task = NULL;
    ep->refcount    = 1;

    return ep;
}

void epoll_instance_put(epoll_instance_t *ep) {
    if (!ep) return;

    int old = __atomic_fetch_sub(&ep->refcount, 1, __ATOMIC_ACQ_REL);
    if (old > 1)
        return;

    epoll_instance_destroy(ep);
}

void epoll_instance_destroy(epoll_instance_t *ep) {
    if (!ep) return;

    epoll_entry_t *e = ep->entries;
    while (e) {
        epoll_entry_t *next = e->next;
        kfree(e, sizeof(epoll_entry_t));
        e = next;
    }

    kfree(ep, sizeof(epoll_instance_t));
}

static epoll_entry_t *ep_find(epoll_instance_t *ep, int fd) {
    for (epoll_entry_t *e = ep->entries; e; e = e->next)
        if (e->fd == fd)
            return e;
    return NULL;
}

int epoll_ctl_add(epoll_instance_t *ep, int fd, epoll_event_t *ev,
                  poll_table_t *pt) {
    if (!ep || !ev)
        return -EINVAL;

    unsigned long irq = irq_push();
    spinlock_acquire(&ep->lock);

    if (ep_find(ep, fd)) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return -EEXIST;
    }

    epoll_entry_t *entry = kmalloc(sizeof(epoll_entry_t));
    if (!entry) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return -ENOMEM;
    }

    entry->fd         = fd;
    entry->events     = ev->events & ~(EPOLLET | EPOLLONESHOT);
    entry->data       = ev->data;
    entry->flags      = ev->events & (EPOLLET | EPOLLONESHOT);
    entry->ready      = 0;
    entry->ready_next = NULL;
    entry->next       = ep->entries;
    ep->entries       = entry;
    ep->entry_count++;

    spinlock_release(&ep->lock);
    irq_restore(irq);

    if (pt) {
        int rc = poll_table_add_wakeup(pt, ep, fd);
        if (rc != 0) {
            irq = irq_push();
            spinlock_acquire(&ep->lock);
            if (ep->entries == entry) {
                ep->entries = entry->next;
            } else {
                epoll_entry_t *prev = ep->entries;
                while (prev && prev->next != entry)
                    prev = prev->next;
                if (prev)
                    prev->next = entry->next;
            }
            ep->entry_count--;
            spinlock_release(&ep->lock);
            irq_restore(irq);
            kfree(entry, sizeof(epoll_entry_t));
            return rc;
        }
    }

    return 0;
}

int epoll_ctl_mod(epoll_instance_t *ep, int fd, epoll_event_t *ev) {
    if (!ep || !ev)
        return -EINVAL;

    unsigned long irq = irq_push();
    spinlock_acquire(&ep->lock);

    epoll_entry_t *entry = ep_find(ep, fd);
    if (!entry) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return -ENOENT;
    }

    entry->events = ev->events & ~(EPOLLET | EPOLLONESHOT);
    entry->data   = ev->data;
    entry->flags  = ev->events & (EPOLLET | EPOLLONESHOT);

    spinlock_release(&ep->lock);
    irq_restore(irq);
    return 0;
}

int epoll_ctl_del(epoll_instance_t *ep, int fd, poll_table_t *pt) {
    if (!ep)
        return -EINVAL;

    unsigned long irq = irq_push();
    spinlock_acquire(&ep->lock);

    epoll_entry_t *prev = NULL;
    epoll_entry_t *cur  = ep->entries;
    while (cur && cur->fd != fd) {
        prev = cur;
        cur  = cur->next;
    }

    if (!cur) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return -ENOENT;
    }

    if (prev)
        prev->next = cur->next;
    else
        ep->entries = cur->next;
    ep->entry_count--;

    cur->events = 0;

    spinlock_release(&ep->lock);
    irq_restore(irq);

    if (pt)
        poll_table_remove_wakeup(pt, ep, fd);

    if (!cur->ready) {
        kfree(cur, sizeof(epoll_entry_t));
    } else {
        cur->fd = -1;
    }

    return 0;
}

static void sched_sleep_interruptible(task_t *task) {
    sched_block(task);
}

static void sched_sleep_timeout(task_t *task, uint64_t ms) {
    uint64_t deadline = epoll_ms_now() + ms;

    while (epoll_ms_now() < deadline) {
        // block if woken early?
        sched_block(task);
    }
}

static uint32_t ep_poll_fd(task_t *task, int fd) {
    (void)fd;

    uint32_t ready = 0;
    if (task->ipc_head)
        ready |= EPOLLIN;
    ready |= EPOLLOUT;
    return ready;
}

int epoll_wait_events(epoll_instance_t *ep, task_t *task,
                      epoll_event_t *out, int maxevents, int timeout_ms) {
    if (!ep || !task || !out || maxevents <= 0)
        return -EINVAL;

    uint64_t deadline = 0;
    if (timeout_ms > 0)
        deadline = epoll_ms_now() + (uint64_t)timeout_ms;

retry:;
    unsigned long irq = irq_push();
    spinlock_acquire(&ep->lock);

    int collected = 0;

    epoll_entry_t *cur      = ep->ready_head;
    epoll_entry_t *new_head = NULL;
    epoll_entry_t *new_tail = NULL;

    ep->ready_head  = NULL;
    ep->ready_tail  = NULL;
    ep->ready_count = 0;

    while (cur && collected < maxevents) {
        epoll_entry_t *next = cur->ready_next;
        cur->ready_next = NULL;
        cur->ready      = 0;

        if (cur->fd == -1) {
            kfree(cur, sizeof(epoll_entry_t));
            cur = next;
            continue;
        }

        if (cur->events == 0) {
            cur = next;
            continue;
        }

        uint32_t pending = ep_poll_fd(task, cur->fd) & cur->events;
        if (!pending) {
            cur = next;
            continue;
        }

        out[collected].events = pending;
        out[collected].data   = cur->data;
        collected++;

        if (cur->flags & EPOLLONESHOT) {
            cur->events = 0;
        } else if (cur->flags & EPOLLET) {
            // dont re-enqueue in any situation with this event flag
        } else {
            cur->ready      = 1;
            cur->ready_next = NULL;
            if (!new_tail) {
                new_head = cur;
                new_tail = cur;
            } else {
                new_tail->ready_next = cur;
                new_tail = cur;
            }
            ep->ready_count++;
        }

        cur = next;
    }

    while (cur) {
        epoll_entry_t *next = cur->ready_next;
        if (cur->fd == -1) {
            kfree(cur, sizeof(epoll_entry_t));
            cur = next;
            continue;
        }
        cur->ready_next = NULL;
        if (!new_tail) {
            new_head = cur;
            new_tail = cur;
        } else {
            new_tail->ready_next = cur;
            new_tail = cur;
        }
        ep->ready_count++;
        cur = next;
    }

    ep->ready_head = new_head;
    ep->ready_tail = new_tail;

    if (collected > 0) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return collected;
    }

    if (timeout_ms == 0) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return 0;
    }

    if (timeout_ms > 0 && epoll_ms_now() >= deadline) {
        spinlock_release(&ep->lock);
        irq_restore(irq);
        return 0;
    }

    ep->waiter_task = task;
    spinlock_release(&ep->lock);
    irq_restore(irq);

    if (timeout_ms < 0) {
        sched_sleep_interruptible(task);
    } else {
        uint64_t now = epoll_ms_now();
        if (now < deadline)
            sched_sleep_timeout(task, deadline - now);
    }

    irq = irq_push();
    spinlock_acquire(&ep->lock);
    if (ep->waiter_task == task)
        ep->waiter_task = NULL;
    spinlock_release(&ep->lock);
    irq_restore(irq);

    goto retry;
}

void ipc_poll_notify_readable(task_t *task) {
    if (!task) return;
    poll_notify(&task->ipc_poll, EPOLLIN);
}

void ipc_poll_check_writable(task_t *task) {
    if (!task) return;
    poll_notify(&task->ipc_poll, EPOLLOUT);
}