#include <syscalls/syscall.h>
#include <sched/scheduler.h>
#include <cpu/percpu.h>
#include <mm/spinlock.h>
#include <user/user_copy.h>
#include <user/user_cpu.h>
#include <user/errno.h>

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

long sys_cpuinfo(uint64_t index, uint64_t user_info_ptr) {
    task_t *caller = get_current_task();
    if (!caller)
        return -ESRCH;
    if (!user_info_ptr)
        return -EFAULT;
    if (index >= cpu_count)
        return -ENOENT;

    cpu_t *cpu = &cpus[index];
    user_cpu_info_t info = {0};

    // its turn so far counts too, or a cpu stuck on one task would look like it did nothing
    unsigned long flags = irq_push();
    task_t *current  = cpu->current;
    uint64_t ran     = rdtsc() - cpu->switch_tsc;
    info.id          = cpu->id;
    info.lapic_id    = cpu->lapic_id;
    info.busy_time   = cpu->busy_tsc;
    info.idle_time   = cpu->idle_tsc;
    info.current_pid = current ? current->id : 0;
    if (current && current == cpu->idle_task)
        info.idle_time += ran;
    else
        info.busy_time += ran;
    irq_restore(flags);

    if (copy_to_user(caller, (void *)user_info_ptr, &info, sizeof(info)) != 0)
        return -EFAULT;
    return 0;
}
