#include <sched/scheduler.h>
#include <string.h>
#include <mm/kalloc.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/cow.h>
#include <mm/tlb.h>
#include <mm/userspace/mmap.h>
#include <user/errno.h>
#include <fs/vfs.h>
#include <fs/memfd.h>

// take the node tracking addr out of the tasks alloc list, the caller owns it after this
static user_alloc_t *alloc_list_unlink(task_t *task, void *addr) {
    user_alloc_t *prev = NULL;

    for (user_alloc_t *a = task->group->alloc_list; a; prev = a, a = a->next) {
        if (a->vaddr != addr)
            continue;

        if (prev) prev->next = a->next;
        else task->group->alloc_list = a->next;
        return a;
    }
    return NULL;
}

/// @brief find a free run in the mmap window and track it in the alloc list, nothing is mapped yet
/// @return base of the reserved range, NULL if it doesnt fit
void* task_mmap_reserve(task_t* task, size_t pages) {
    if (!task || !pages) return NULL;

    uintptr_t base = USER_MMAP_BASE;
    user_alloc_t* prev = NULL;
    user_alloc_t* next = task->group->alloc_list;

    // the list is kept sorted so the first gap that fits is the lowest one
    for (user_alloc_t* a = task->group->alloc_list; a; prev = a, a = a->next) {
        size_t gap_pages = ((uintptr_t)a->vaddr - base) / PAGE_SIZE;

        if (gap_pages >= pages) {
            next = a;
            break;
        }

        base = (uintptr_t)a->vaddr + (a->pages * PAGE_SIZE);
        next = a->next;
    }

    if (base > USER_MMAP_LIMIT)
        return NULL;
    if (pages > (USER_MMAP_LIMIT - base) / PAGE_SIZE)
        return NULL;

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc)
        return NULL;

    alloc->vaddr = (void*)base;
    alloc->pages = pages;
    alloc->prot    = PROT_READ | PROT_WRITE;
    alloc->flags   = 0;
    alloc->backing = NULL;
    alloc->offset  = 0;
    alloc->next = next;

    if (prev)
        prev->next = alloc;
    else
        task->group->alloc_list = alloc;

    return (void*)base;
}

/// @brief like task_mmap_reserve but at addr, for things that have to live somewhere exact
/// @return addr, NULL if any of it is already taken or outside the mmap window
void* task_mmap_reserve_at(task_t* task, uintptr_t addr, size_t pages) {
    if (!task || !pages || (addr & (PAGE_SIZE - 1)))
        return NULL;
    if (addr < USER_MMAP_BASE || addr > USER_MMAP_LIMIT)
        return NULL;
    if (pages > (USER_MMAP_LIMIT - addr) / PAGE_SIZE)
        return NULL;

    uintptr_t end = addr + pages * PAGE_SIZE;
    user_alloc_t* prev = NULL;
    user_alloc_t* next = task->group->alloc_list;

    for (; next; prev = next, next = next->next) {
        uintptr_t a_start = (uintptr_t)next->vaddr;
        uintptr_t a_end   = a_start + next->pages * PAGE_SIZE;

        if (a_start >= end)
            break;
        if (a_end > addr)
            return NULL;
    }

    user_alloc_t* alloc = kmalloc(sizeof(user_alloc_t));
    if (!alloc)
        return NULL;

    alloc->vaddr = (void*)addr;
    alloc->pages = pages;
    alloc->prot    = PROT_READ | PROT_WRITE;
    alloc->flags   = 0;
    alloc->backing = NULL;
    alloc->offset  = 0;
    alloc->next = next;

    if (prev)
        prev->next = alloc;
    else
        task->group->alloc_list = alloc;

    return (void*)addr;
}

/// @brief forget a reservation, whatever is mapped there is left alone
void task_mmap_release(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t *a = alloc_list_unlink(task, addr);
    if (a) user_alloc_free(a);
}

#define MMAP_RUN_MAX    64

void* task_mmap(task_t* task, size_t pages) {
    uintptr_t base = (uintptr_t)task_mmap_reserve(task, pages);
    if (!base) return NULL;

    size_t done = 0;
    size_t want = MMAP_RUN_MAX;

    while (done < pages) {
        if (want > pages - done)
            want = pages - done;

        // fall back to smaller ones when fragmented
        paddr_t phys = want > 1 ? pmm_try_alloc_pages(want) : pmm_alloc_pages(1);
        if (!phys && want > 1) {
            want /= 2;
            continue;
        }

        // zeroed, a task should never see what the last owner of the frame left behind
        if (phys)
            memset(paddr_to_vaddr(phys), 0, want * PAGE_SIZE);

        void *at = (void *)(base + done * PAGE_SIZE);
        if (!phys || paging_map_range(task->page_map, phys, (uint64_t)at, want,
                                      PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX) != 0) {
            if (phys) {
                vmm_unmap_pages(task->page_map, at, want);
                pmm_free_pages(phys, want);
            }
            if (done) (void)vmm_unmap_free_pages(task->page_map, (void *)base, done);
            task_mmap_release(task, (void *)base);
            return NULL;
        }

        done += want;
    }

    return (void*)base;
}

void task_munmap(task_t* task, void* addr) {
    if (!task || !addr) return;

    user_alloc_t *a = alloc_list_unlink(task, addr);
    if (!a) return;

    (void)vmm_unmap_free_pages(task->page_map, addr, a->pages);
    user_alloc_free(a);
}

// vmas, the alloc list nodes with a prot and flags on them

static inline uintptr_t alloc_end(const user_alloc_t *a) {
    return (uintptr_t)a->vaddr + a->pages * PAGE_SIZE;
}

static bool writes_memfd(const user_alloc_t *a) {
    return (a->flags & UALLOC_SHARED) && (a->prot & PROT_WRITE) && memfd_is(a->backing);
}

void user_alloc_free(user_alloc_t *a) {
    if (a->backing) {
        if (writes_memfd(a))
            memfd_track_writer(a->backing, -1);
        vfs_drop(a->backing);
    }
    kfree(a);
}

void user_alloc_copy(user_alloc_t *dst, const user_alloc_t *src) {
    dst->vaddr   = src->vaddr;
    dst->pages   = src->pages;
    dst->prot    = src->prot;
    dst->flags   = src->flags;
    dst->backing = src->backing;
    dst->offset  = src->offset;
    if (dst->backing) {
        vfs_hold(dst->backing);
        if (writes_memfd(dst))
            memfd_track_writer(dst->backing, 1);
    }
}

static user_alloc_t *alloc_find(task_t *task, uintptr_t addr) {
    for (user_alloc_t *a = task->group->alloc_list; a; a = a->next) {
        if ((uintptr_t)a->vaddr > addr)
            break;
        if (addr < alloc_end(a))
            return a;
    }
    return NULL;
}

// a fault at addr hit something mapped (PROT_NONE, or a write to a read only range)
int task_addr_mapped(struct task *task, uintptr_t addr) {
    return task && task->group && alloc_find(task, addr) != NULL;
}

// what file addr came from and how far in, for crash reports. 0 when its not a file mapping
int task_addr_file(struct task *task, uintptr_t addr, const char **name, uint64_t *off) {
    if (!task || !task->group)
        return 0;
    user_alloc_t *a = alloc_find(task, addr);
    if (!a || !a->backing)
        return 0;
    *name = a->backing->name;
    *off  = a->offset + (addr - (uintptr_t)a->vaddr);
    return 1;
}

// make a node start at at, if one runs across it
static int alloc_split(task_t *task, uintptr_t at) {
    user_alloc_t *a = alloc_find(task, at);
    if (!a || (uintptr_t)a->vaddr == at)
        return 0;

    user_alloc_t *tail = kmalloc(sizeof(user_alloc_t));
    if (!tail)
        return -ENOMEM;

    user_alloc_copy(tail, a);
    tail->vaddr  = (void *)at;
    tail->pages  = (alloc_end(a) - at) / PAGE_SIZE;
    tail->offset = a->offset + (at - (uintptr_t)a->vaddr);
    tail->next   = a->next;
    a->pages    = (at - (uintptr_t)a->vaddr) / PAGE_SIZE;
    a->next     = tail;
    return 0;
}

static uint64_t prot_pte(int prot) {
    if (!(prot & (PROT_READ | PROT_WRITE | PROT_EXEC)))
        return PTE_PROTNONE | PTE_NX;

    uint64_t f = PTE_USER;
    if (prot & PROT_WRITE)
        f |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC))
        f |= PTE_NX;
    return f;
}

int task_fault_in(task_t *task, uintptr_t addr, int write, int exec) {
    if (!task || !task->group || task->task_privilege != P_USER)
        return 0;

    user_alloc_t *a = alloc_find(task, addr);
    if (!a || !(a->flags & UALLOC_LAZY))
        return 0;
    if (!(a->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)))
        return 0;
    if ((write && !(a->prot & PROT_WRITE)) || (exec && !(a->prot & PROT_EXEC)))
        return 0;

    uintptr_t page = PAGE_ALIGN_DOWN(addr);
    uint64_t *pte = paging_get_page(task->page_map, page, 0);
    if (pte && (*pte & PTE_PRESENT))
        return 0;

    uint64_t at = a->offset + (page - (uintptr_t)a->vaddr);

    // a memfds own page, mapped shared so everyone with it sees the same bytes
    if (a->flags & UALLOC_SHARED) {
        paddr_t frame = memfd_page(a->backing, at / PAGE_SIZE, true);
        if (!frame)
            return 0;
        cow_ref_page(frame);
        paging_map_page_invl(task->page_map, frame, page, prot_pte(a->prot) | PTE_SHARED, 0);
        pte = paging_get_page(task->page_map, page, 0);
        if (!pte || !(*pte & PTE_PRESENT)) {
            cow_unref_page(frame);
            return 0;
        }
        return 1;
    }

    void   *mem;
    paddr_t phys = paging_alloc_empty_frame(&mem);
    if (!phys)
        return 0;

    // a private copy of the file, whats past its end stays zero
    if (a->backing && inode_read(a->backing, mem, PAGE_SIZE, at) < 0) {
        pmm_free_pages(phys, 1);
        return 0;
    }
    paging_map_page_invl(task->page_map, phys, page, prot_pte(a->prot), 0);

    pte = paging_get_page(task->page_map, page, 0);
    if (!pte || !(*pte & PTE_PRESENT)) {
        pmm_free_pages(phys, 1);
        return 0;
    }
    return 1;
}

long task_vma_unmap(task_t *task, uintptr_t addr, size_t len) {
    uintptr_t end = addr + len;
    if (alloc_split(task, addr) || alloc_split(task, end))
        return -ENOMEM;

    user_alloc_t *prev = NULL;
    user_alloc_t *a    = task->group->alloc_list;
    while (a && (uintptr_t)a->vaddr < end) {
        user_alloc_t *next = a->next;
        if ((uintptr_t)a->vaddr >= addr) {
            if (prev)
                prev->next = next;
            else
                task->group->alloc_list = next;
            (void)vmm_unmap_free_pages(task->page_map, a->vaddr, a->pages);
            user_alloc_free(a);
        } else {
            prev = a;
        }
        a = next;
    }
    return 0;
}

// shared ones are filled now
static int fill_shared(task_t *task, uintptr_t base, size_t pages, int prot) {
    for (size_t i = 0; i < pages; i++) {
        paddr_t phys = paging_alloc_empty_frame(NULL);
        if (!phys)
            return -ENOMEM;
        cow_ref_page(phys);
        paging_map_page_invl(task->page_map, phys, base + i * PAGE_SIZE, prot_pte(prot) | PTE_SHARED, 0);

        uint64_t *pte = paging_get_page(task->page_map, base + i * PAGE_SIZE, 0);
        if (!pte || !(*pte & PTE_PRESENT)) {
            cow_unref_page(phys);
            pmm_free_pages(phys, 1);
            return -ENOMEM;
        }
    }
    return 0;
}

long task_vma_map(task_t *task, uintptr_t hint, size_t len, int prot, int flags) {
    size_t    pages = len / PAGE_SIZE;
    uintptr_t base  = 0;
    if ((flags & MAP_TYPE) == MAP_PRIVATE && (prot & PROT_WRITE) && !(flags & MAP_NORESERVE) &&
        len > paging_get_usable_mem_size())
        return -ENOMEM;

    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (hint & (PAGE_SIZE - 1))
            return -EINVAL;
        if (hint < USER_MMAP_BASE || hint > USER_MMAP_LIMIT || pages > (USER_MMAP_LIMIT - hint) / PAGE_SIZE)
            return -ENOMEM;

        if (!(flags & MAP_FIXED_NOREPLACE)) {
            long r = task_vma_unmap(task, hint, len);
            if (r < 0)
                return r;
        }
        base = (uintptr_t)task_mmap_reserve_at(task, hint, pages);
        if (!base)
            return (flags & MAP_FIXED_NOREPLACE) ? -EEXIST : -ENOMEM;
    } else {
        if (hint)
            base = (uintptr_t)task_mmap_reserve_at(task, PAGE_ALIGN_DOWN(hint), pages);
        if (!base)
            base = (uintptr_t)task_mmap_reserve(task, pages);
        if (!base)
            return -ENOMEM;
    }

    user_alloc_t *a = alloc_find(task, base);
    a->prot = prot;

    if ((flags & MAP_TYPE) != MAP_PRIVATE) {
        if (fill_shared(task, base, pages, prot) < 0) {
            task_vma_unmap(task, base, len);
            return -ENOMEM;
        }
        return (long)base;
    }

    a->flags = UALLOC_LAZY;
    if ((flags & MAP_POPULATE) && (prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {
        for (size_t i = 0; i < pages; i++) {
            if (!task_fault_in(task, base + i * PAGE_SIZE, 0, 0)) {
                task_vma_unmap(task, base, len);
                return -ENOMEM;
            }
        }
    }
    return (long)base;
}

long task_vma_map_file(task_t *task, uintptr_t hint, size_t len, int prot, int flags,
                       struct INode *inode, uint64_t offset) {
    bool shared = (flags & MAP_TYPE) != MAP_PRIVATE;
    if (shared && memfd_is(inode) && (prot & PROT_WRITE)) {
        int err = memfd_track_writer(inode, 1);
        if (err)
            return err;
        memfd_track_writer(inode, -1);
    }

    // mapped anonymous first then pointed at the file, MAP_PRIVATE so nothing gets filled
    long base = task_vma_map(task, hint, len, prot, (flags & ~MAP_TYPE & ~MAP_POPULATE & ~MAP_KERNEL_NOWRITE) | MAP_PRIVATE | MAP_NORESERVE);
    if (base < 0)
        return base;

    user_alloc_t *a = alloc_find(task, (uintptr_t)base);
    a->backing = inode;
    a->offset  = offset;
    a->flags   = UALLOC_LAZY | (shared && memfd_is(inode) ? UALLOC_SHARED : 0);
    if (shared && (flags & MAP_KERNEL_NOWRITE))
        a->flags |= UALLOC_NOWRITE;
    vfs_hold(inode);
    if (writes_memfd(a))
        memfd_track_writer(inode, 1);

    if ((flags & MAP_POPULATE) && (prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) {
        for (size_t i = 0; i < len / PAGE_SIZE; i++)
            task_fault_in(task, (uintptr_t)base + i * PAGE_SIZE, 0, 0);
    }
    return base;
}

// the ranges page tables, a whole missing table gets stepped over
static void vma_each_pte(task_t *task, uintptr_t start, uintptr_t end,
                         void (*fn)(task_t *, uintptr_t, uint64_t *, int), int arg) {
    uintptr_t va = start;
    while (va < end) {
        uint64_t *pt = paging_get_table(task->page_map, va);
        uintptr_t table_end = (va & ~((uintptr_t)(512 * PAGE_SIZE) - 1)) + 512 * PAGE_SIZE;
        if (table_end > end)
            table_end = end;
        if (pt) {
            for (; va < table_end; va += PAGE_SIZE) {
                uint64_t *pte = &pt[(va >> 12) & 0x1FF];
                if (*pte & PTE_PRESENT)
                    fn(task, va, pte, arg);
            }
        }
        va = table_end;
    }
}

static void protect_pte(task_t *task, uintptr_t va, uint64_t *pte, int prot) {
    if (!(*pte & (PTE_USER | PTE_PROTNONE)))
        return;

    // a cow page would get write back on its next fault, give it its own frame now
    if (!(prot & PROT_WRITE) && (*pte & PTE_COW)) {
        if (!paging_handle_cow_fault(task, va, 0x7))
            return;
        pte = paging_get_page(task->page_map, va, 0);
    }

    uint64_t e = *pte & ~(PTE_USER | PTE_PROTNONE | PTE_WRITABLE | PTE_NX);
    uint64_t f = prot_pte(prot);
    if (*pte & PTE_COW)
        f &= ~PTE_WRITABLE;
    *pte = e | f;
    asm volatile("invlpg (%0)" :: "r"(va) : "memory");
}

long task_vma_protect(task_t *task, uintptr_t addr, size_t len, int prot) {
    uintptr_t end = addr + len;

    // every page has to be in an mmap
    uintptr_t at = addr;
    for (user_alloc_t *a = task->group->alloc_list; a && at < end; a = a->next) {
        if (alloc_end(a) <= at)
            continue;
        if ((uintptr_t)a->vaddr > at)
            break;
        at = alloc_end(a);
    }
    if (at < end)
        return -ENOMEM;

    if (alloc_split(task, addr) || alloc_split(task, end))
        return -ENOMEM;

    if (prot & PROT_WRITE) {
        for (user_alloc_t *a = alloc_find(task, addr); a && (uintptr_t)a->vaddr < end; a = a->next)
            if (a->flags & UALLOC_NOWRITE)
                return -EACCES;
    }

    for (user_alloc_t *a = alloc_find(task, addr); a && (uintptr_t)a->vaddr < end; a = a->next) {
        bool was = writes_memfd(a);
        uint32_t old = a->prot;
        a->prot = prot;
        bool now = writes_memfd(a);
        if (now && !was && memfd_track_writer(a->backing, 1) < 0) {
            a->prot = old;
            return -EPERM;
        }
        if (was && !now)
            memfd_track_writer(a->backing, -1);
    }

    vma_each_pte(task, addr, end, protect_pte, prot);
    tlb_shootdown(task->page_map);
    return 0;
}

long task_vma_discard(task_t *task, uintptr_t addr, size_t len) {
    uintptr_t end = addr + len;

    for (user_alloc_t *a = task->group->alloc_list; a && (uintptr_t)a->vaddr < end; a = a->next) {
        if (!(a->flags & UALLOC_LAZY) || alloc_end(a) <= addr)
            continue;
        uintptr_t s = (uintptr_t)a->vaddr > addr ? (uintptr_t)a->vaddr : addr;
        uintptr_t e = alloc_end(a) < end ? alloc_end(a) : end;
        (void)vmm_unmap_free_pages(task->page_map, (void *)s, (e - s) / PAGE_SIZE);
    }
    return 0;
}
