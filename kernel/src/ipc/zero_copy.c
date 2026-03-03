#include <ipc/zero_copy.h>

#include <mm/kalloc.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <mm/spinlock.h>
#include <mm/userspace/mmap.h>
#include <sched/scheduler.h>
#include <user/errno.h>
#include <user/user_copy.h>
#include <user/user_ipc.h>

static spinlock_t ipc_lock;
static int ipc_lock_init_done;

static void ipc_init_lock(void) {
    if (ipc_lock_init_done)
        return;
    spinlock_init(&ipc_lock);
    ipc_lock_init_done = 1;
}

static int ipc_range_in_alloc_list(task_t *task, uintptr_t addr, size_t pages) {
    if (!task || !addr || !pages)
        return 0;

    uintptr_t range_end = addr + pages * PAGE_SIZE;
    if (range_end < addr)
        return 0;

    for (user_alloc_t *alloc = task->alloc_list; alloc; alloc = alloc->next) {
        uintptr_t alloc_start = (uintptr_t)alloc->vaddr;
        uintptr_t alloc_end = alloc_start + alloc->pages * PAGE_SIZE;
        if (addr >= alloc_start && range_end <= alloc_end)
            return 1;
    }

    return 0;
}

static int ipc_detach_alloc_range(task_t *task, uintptr_t addr, size_t pages) {
    if (!task || !addr || !pages)
        return -EINVAL;

    uintptr_t range_end = addr + pages * PAGE_SIZE;
    if (range_end < addr)
        return -EINVAL;

    user_alloc_t *prev = NULL;
    user_alloc_t *cur = task->alloc_list;
    while (cur) {
        uintptr_t alloc_start = (uintptr_t)cur->vaddr;
        uintptr_t alloc_end = alloc_start + cur->pages * PAGE_SIZE;

        if (addr < alloc_start || range_end > alloc_end) {
            prev = cur;
            cur = cur->next;
            continue;
        }

        if (addr == alloc_start && range_end == alloc_end) {
            if (prev)
                prev->next = cur->next;
            else
                task->alloc_list = cur->next;
            kfree(cur, sizeof(user_alloc_t));
            return 0;
        }

        if (addr == alloc_start) {
            cur->vaddr = (void *)range_end;
            cur->pages -= pages;
            return 0;
        }

        if (range_end == alloc_end) {
            cur->pages -= pages;
            return 0;
        }

        size_t prefix_pages = (addr - alloc_start) / PAGE_SIZE;
        size_t suffix_pages = (alloc_end - range_end) / PAGE_SIZE;

        user_alloc_t *tail = kmalloc(sizeof(user_alloc_t));
        if (!tail)
            return -ENOMEM;

        tail->vaddr = (void *)range_end;
        tail->pages = suffix_pages;
        tail->next = cur->next;

        cur->pages = prefix_pages;
        cur->next = tail;
        return 0;
    }

    return -EINVAL;
}

static void ipc_queue_push(task_t *task, ipc_message_t *msg) {
    msg->next = NULL;
    if (!task->ipc_tail) {
        task->ipc_head = msg;
        task->ipc_tail = msg;
        return;
    }

    task->ipc_tail->next = msg;
    task->ipc_tail = msg;
}

static ipc_message_t *ipc_queue_pop(task_t *task) {
    ipc_message_t *msg = task->ipc_head;
    if (!msg)
        return NULL;

    task->ipc_head = msg->next;
    if (!task->ipc_head)
        task->ipc_tail = NULL;
    msg->next = NULL;
    return msg;
}

static int ipc_validate_send_range(task_t *sender, uintptr_t src_addr, size_t pages) {
    if (!sender || !src_addr || !pages)
        return -EINVAL;
    if (src_addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (!ipc_range_in_alloc_list(sender, src_addr, pages))
        return -EINVAL;

    for (size_t i = 0; i < pages; i++) {
        uintptr_t page = src_addr + i * PAGE_SIZE;
        uint64_t *pte = paging_get_page(sender->page_map, page, 0);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER))
            return -EFAULT;
    }

    return 0;
}

long ipc_send_pages(task_t *sender, uint64_t target_pid, uint64_t src_addr, uint64_t pages) {
    if (!sender)
        return -ESRCH;
    if (!target_pid || !src_addr || !pages)
        return -EINVAL;

    task_t *target = sched_get_task(target_pid);
    if (!target ||
        target->state == TASK_FREE ||
        target->state == TASK_DEAD ||
        target->state == TASK_ZOMBIE)
        return -ESRCH;
    if (target->task_privilege != P_USER || sender->task_privilege != P_USER)
        return -EPERM;

    int valid = ipc_validate_send_range(sender, (uintptr_t)src_addr, (size_t)pages);
    if (valid != 0)
        return valid;

    ipc_message_t *msg = kmalloc(sizeof(ipc_message_t));
    if (!msg)
        return -ENOMEM;

    paddr_t *phys_pages = kmalloc((size_t)pages * sizeof(paddr_t));
    if (!phys_pages) {
        kfree(msg, sizeof(ipc_message_t));
        return -ENOMEM;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t src_page = (uintptr_t)src_addr + i * PAGE_SIZE;
        uint64_t *src_pte = paging_get_page(sender->page_map, src_page, 0);
        if (!src_pte || !(*src_pte & PTE_PRESENT)) {
            kfree(phys_pages, (size_t)pages * sizeof(paddr_t));
            kfree(msg, sizeof(ipc_message_t));
            return -EFAULT;
        }
        phys_pages[i] = (paddr_t)(*src_pte & PADDR_ENTRY_MASK);
    }

    int detach_rc = ipc_detach_alloc_range(sender, (uintptr_t)src_addr, (size_t)pages);
    if (detach_rc != 0) {
        kfree(phys_pages, (size_t)pages * sizeof(paddr_t));
        kfree(msg, sizeof(ipc_message_t));
        return detach_rc;
    }

    for (size_t i = 0; i < pages; i++) {
        uintptr_t src_page = (uintptr_t)src_addr + i * PAGE_SIZE;
        paging_unmap_page(sender->page_map, src_page);
    }

    msg->sender_pid = sender->id;
    msg->pages = pages;
    msg->flags = 0;
    msg->phys_pages = phys_pages;

    ipc_init_lock();
    unsigned long irq_flags = irq_push();
    spinlock_acquire(&ipc_lock);
    ipc_queue_push(target, msg);
    spinlock_release(&ipc_lock);
    irq_restore(irq_flags);

    return 0;
}

long ipc_recv(task_t *receiver, uint64_t user_msg_ptr) {
    if (!receiver)
        return -ESRCH;
    if (!user_msg_ptr || !user_ptr_valid(user_msg_ptr))
        return -EFAULT;

    ipc_init_lock();
    unsigned long irq_flags = irq_push();
    spinlock_acquire(&ipc_lock);
    ipc_message_t *msg = ipc_queue_pop(receiver);
    spinlock_release(&ipc_lock);
    irq_restore(irq_flags);

    if (!msg)
        return -EAGAIN;

    void *target_addr = task_mmap(receiver, msg->pages);
    if (!target_addr) {
        for (size_t i = 0; i < (size_t)msg->pages; i++)
            pmm_free_pages(msg->phys_pages[i], 1);
        kfree(msg->phys_pages, (size_t)msg->pages * sizeof(paddr_t));
        kfree(msg, sizeof(ipc_message_t));
        return -ENOMEM;
    }

    for (size_t i = 0; i < (size_t)msg->pages; i++) {
        uintptr_t dst_page = (uintptr_t)target_addr + i * PAGE_SIZE;
        paging_map_page_invl(receiver->page_map, msg->phys_pages[i], dst_page, PTE_USER | PTE_WRITABLE, 0);
    }

    user_ipc_msg_t out = {
        .sender_pid = msg->sender_pid,
        .addr = (uint64_t)(uintptr_t)target_addr,
        .pages = msg->pages,
        .flags = msg->flags
    };

    if (copy_to_user(receiver, (void *)user_msg_ptr, &out, sizeof(out)) != 0) {
        task_munmap(receiver, target_addr);
        kfree(msg->phys_pages, (size_t)msg->pages * sizeof(paddr_t));
        kfree(msg, sizeof(ipc_message_t));
        return -EFAULT;
    }

    kfree(msg->phys_pages, (size_t)msg->pages * sizeof(paddr_t));
    kfree(msg, sizeof(ipc_message_t));

    return 0;
}

void ipc_task_cleanup(task_t *task) {
    if (!task)
        return;

    ipc_init_lock();
    unsigned long irq_flags = irq_push();
    spinlock_acquire(&ipc_lock);
    ipc_message_t *msg = task->ipc_head;
    task->ipc_head = NULL;
    task->ipc_tail = NULL;
    spinlock_release(&ipc_lock);
    irq_restore(irq_flags);

    while (msg) {
        ipc_message_t *next = msg->next;
        for (size_t i = 0; i < (size_t)msg->pages; i++)
            pmm_free_pages(msg->phys_pages[i], 1);
        kfree(msg->phys_pages, (size_t)msg->pages * sizeof(paddr_t));
        kfree(msg, sizeof(ipc_message_t));
        msg = next;
    }
}
