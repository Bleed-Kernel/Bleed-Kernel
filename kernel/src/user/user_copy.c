#include <mm/paging.h>
#include <mm/cow.h>
#include <sched/scheduler.h>
#include <stdint.h>
#include <stddef.h>
#include <mm/smap.h>
#include <cpu/control_registers.h>
#include <string.h>
#include <user/user_copy.h>

#define USER_MIN 0x0000000000001000ULL
#define USER_MAX 0x00007fffffffffffULL

int user_ptr_valid(uint64_t ptr) {
    return ptr >= USER_MIN && ptr <= USER_MAX;
}

// the pte of a user page the task can touch
static uint64_t *user_page_pte(task_t *task, uintptr_t page, int write) {
    uint64_t *pte = paging_get_page(task->page_map, page, 0);

    // a bit of stack the task hasnt touched yet is still its to be handed
    if (!pte && paging_handle_stack_fault(task, page))
        pte = paging_get_page(task->page_map, page, 0);
    if (!pte || !(*pte & PTE_USER))
        return NULL;

    // or we end up faulting because we write with bad access
    if (write && !(*pte & PTE_WRITABLE)) {
        if (!paging_handle_cow_fault(task, page, 0x7))
            return NULL;
        pte = paging_get_page(task->page_map, page, 0);
        if (!pte || !(*pte & PTE_WRITABLE))
            return NULL;
    }
    return pte;
}

static int user_range_mapped(task_t *task, uintptr_t addr, size_t len, int write) {
    if (!task || addr < USER_MIN || addr + len < addr || addr + len > USER_MAX)
        return 0;

    uintptr_t start = addr & ~(PAGE_SIZE - 1);
    uintptr_t end   = (addr + len - 1) & ~(PAGE_SIZE - 1);

    for (uintptr_t p = start; p <= end; p += PAGE_SIZE) {
        if (!user_page_pte(task, p, write))
            return 0;
    }

    return 1;
}

int copy_to_user(task_t *user_task, void *udst, const void *src, size_t len) {
    if (!user_task || !udst || !src || len == 0) return -1;
    if (!user_range_mapped(user_task, (uintptr_t)udst, len, 1))
        return -1;

    // almost always the caller writing to itself, a cr3 reload there just throws the tlb away
    paddr_t old_cr3 = read_cr3();
    int     switch_cr3 = (old_cr3 & PADDR_ENTRY_MASK) != (user_task->page_map & PADDR_ENTRY_MASK);
    if (switch_cr3)
        paging_switch_address_space(user_task->page_map);

    uint8_t *dst = (uint8_t *)udst;
    const uint8_t *s = (const uint8_t *)src;

    SMAP_ALLOW{
        memcpy(dst, s, len);
    }

    if (switch_cr3)
        paging_switch_address_space(old_cr3);
    return 0;
}

/// @brief the kernels hhdm view of the user byte at addr, cow resolved first when its for writing
/// @return NULL if its not the tasks to touch or the frame isnt ram the hhdm maps (mmio)
void *user_page_ptr(task_t *task, uintptr_t addr, int write) {
    if (!task || addr < USER_MIN || addr > USER_MAX)
        return NULL;

    uint64_t *pte = user_page_pte(task, addr & ~(PAGE_SIZE - 1), write);
    if (!pte || (*pte & PTE_NOFREE))
        return NULL;
    return (uint8_t *)paddr_to_vaddr(*pte & PADDR_ENTRY_MASK) + (addr & (PAGE_SIZE - 1));
}

int copy_from_user(task_t *user_task, void *kernel_dst, const void *user_src, size_t len) {
    if (!user_task || !kernel_dst || !user_src || len == 0) return -1;

    if (!user_range_mapped(user_task, (uintptr_t)user_src, len, 0))
        return -1;

    char *dst = (char *)kernel_dst;
    const char *src = (const char *)user_src;

    SMAP_ALLOW{
        memcpy(dst, src, len);
    }

    return 0;
}

/// @brief length of a user string, scanned a page at a time instead of byte by byte
/// @return length without the terminator, -1 if unmapped, -2 if no terminator within max_len
long user_strnlen(task_t *caller, const char *user_src, size_t max_len) {
    if (!caller || !user_src) return -1;

    char chunk[256];
    size_t scanned = 0;

    while (scanned < max_len) {
        uintptr_t addr = (uintptr_t)user_src + scanned;
        size_t to_page_end = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        size_t n = to_page_end < sizeof(chunk) ? to_page_end : sizeof(chunk);
        if (n > max_len - scanned)
            n = max_len - scanned;

        if (copy_from_user(caller, chunk, (const void *)addr, n) != 0)
            return -1;

        for (size_t i = 0; i < n; i++) {
            if (chunk[i] == '\0')
                return (long)(scanned + i);
        }
        scanned += n;
    }

    return -2;
}

/// @brief copy a user string into a fixed kernel buffer, only touching bytes up to its terminator
/// @return string length, -1 if unmapped, -2 if it does not fit in dst_len
long copy_user_path(task_t *caller, const char *user_src, char *kernel_dst, size_t dst_len) {
    if (!kernel_dst || dst_len == 0) return -1;

    // one pass straight out of the tasks frames, instead of finding the end then copying again
    size_t done = 0;
    while (done < dst_len) {
        uintptr_t at = (uintptr_t)user_src + done;
        const char *src = user_page_ptr(caller, at, 0);
        if (!src)
            break;

        size_t n = PAGE_SIZE - (at & (PAGE_SIZE - 1));
        if (n > dst_len - done)
            n = dst_len - done;

        for (size_t i = 0; i < n; i++) {
            kernel_dst[done + i] = src[i];
            if (!src[i])
                return (long)(done + i);
        }
        done += n;
    }
    if (done >= dst_len)
        return -2;

    long len = user_strnlen(caller, user_src, dst_len);
    if (len < 0) return len;

    if (copy_from_user(caller, kernel_dst, user_src, (size_t)len + 1) != 0) return -1;
    kernel_dst[len] = '\0';
    return len;
}

/// @brief copy a user string into a fixed kernel buffer
/// @return 0 on success, -1 if its unmapped or doesnt fit
int copy_user_string(task_t *caller, const char *user_src, char *kernel_dst, size_t dst_len) {
    if (dst_len < 2) return -1;

    // used to be a page walk per byte, copy_user_path finds the terminator first
    return copy_user_path(caller, user_src, kernel_dst, dst_len) < 0 ? -1 : 0;
}
