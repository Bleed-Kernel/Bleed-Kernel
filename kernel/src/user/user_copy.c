#include <mm/paging.h>
#include <sched/scheduler.h>
#include <stdint.h>
#include <stddef.h>
#include <mm/smap.h>
#include <cpu/control_registers.h>

#define PAGE_SIZE 4096
#define USER_MIN 0x0000000000001000ULL
#define USER_MAX 0x00007fffffffffffULL

static int user_range_mapped(task_t *task, uintptr_t addr, size_t len) {
    if (!task || addr < USER_MIN || addr + len < addr || addr + len > USER_MAX)
        return 0;

    uintptr_t start = addr & ~(PAGE_SIZE - 1);
    uintptr_t end   = (addr + len - 1) & ~(PAGE_SIZE - 1);

    for (uintptr_t p = start; p <= end; p += PAGE_SIZE) {
        uint64_t *pte = paging_get_page(task->page_map, p, 0);
        if (!pte) return 0;
        if (!(*pte & 0x1)) return 0; // present
        if (!(*pte & 0x4)) return 0; // user
    }

    return 1;
}

int copy_to_user(task_t *user_task, void *udst, const void *src, size_t len) {
    if (!user_task || !udst || !src || len == 0) return -1;

    // switch to user task's page table
    paddr_t old_cr3 = read_cr3();
    paging_switch_address_space(user_task->page_map);

    uint8_t *dst = (uint8_t *)udst;
    const uint8_t *s = (const uint8_t *)src;

    SMAP_ALLOW{
        for (size_t i = 0; i < len; i++) {
            if (!user_range_mapped(user_task, (uintptr_t)(dst + i), 1)) {
                paging_switch_address_space(old_cr3);
                return -1;
            }
            dst[i] = s[i];
        }
    }

    paging_switch_address_space(old_cr3);
    return 0;
}

int copy_from_user(task_t *user_task, void *kernel_dst, const void *user_src, size_t len) {
    if (!user_task || !kernel_dst || !user_src || len == 0) return -1;

    if (!user_range_mapped(user_task, (uintptr_t)user_src, len))
        return -1;

    char *dst = (char *)kernel_dst;
    const char *src = (const char *)user_src;

    SMAP_ALLOW{
        for (size_t i = 0; i < len; i++)
            dst[i] = ((volatile char *)src)[i];
    }

    return 0;
}
