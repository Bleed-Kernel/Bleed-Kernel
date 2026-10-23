#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <mm/paging.h>
#include <user/user_copy.h>
#include <user/errno.h>

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

long sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    task_t *task = get_current_task();
    if (!task)
        return -ESRCH;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (prot & ~(uint64_t)(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    // no way to make a user page unreadable without it looking like a kernel one
    if (!prot)
        return -EINVAL;
    if (!len)
        return 0;

    uint64_t end;
    if (__builtin_add_overflow(addr, len, &end))
        return -ENOMEM;
    end = PAGE_ALIGN_UP(end);
    if (!user_ptr_valid(addr) || !user_ptr_valid(end - 1))
        return -ENOMEM;

    // all or nothing, check every page is there before touching any
    for (uint64_t page = addr; page < end; page += PAGE_SIZE) {
        uint64_t *pte = paging_get_page(task->page_map, page, 0);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER))
            return -ENOMEM;
    }

    for (uint64_t page = addr; page < end; page += PAGE_SIZE) {
        uint64_t *pte = paging_get_page(task->page_map, page, 0);

        // a cow page would get write back on its next fault, give it its own frame now
        if (!(prot & PROT_WRITE) && (*pte & PTE_COW)) {
            if (!paging_handle_cow_fault(task, page, 0x7))
                return -ENOMEM;
            pte = paging_get_page(task->page_map, page, 0);
        }

        uint64_t e = *pte;
        if (prot & PROT_WRITE) {
            if (!(e & PTE_COW))
                e |= PTE_WRITABLE;
        } else {
            e &= ~PTE_WRITABLE;
        }

        if (prot & PROT_EXEC)
            e &= ~PTE_NX;
        else
            e |= PTE_NX;

        *pte = e;
        asm volatile("invlpg (%0)" :: "r"(page) : "memory");
    }

    return 0;
}
