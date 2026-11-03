#include <mm/tlb.h>
#include <cpu/percpu.h>
#include <cpu/control_registers.h>
#include <ACPI/acpi_apic.h>
#include <sched/scheduler.h>

void tlb_flush_check(void) {
    cpu_t *cpu = this_cpu();
    if (!__atomic_load_n(&cpu->tlb_flush, __ATOMIC_ACQUIRE))
        return;

    paging_switch_address_space(read_cr3());
    __atomic_store_n(&cpu->tlb_flush, 0, __ATOMIC_RELEASE);
}

// only ever called with the bkl held, so two of these never wait on each other
void tlb_shootdown(paddr_t cr3) {
    if (cpu_count < 2)
        return;

    cr3 &= PADDR_ENTRY_MASK;
    cpu_t   *self    = this_cpu();
    uint64_t pending = 0;

    // the ptes have to be out before we look at who has them loaded
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    for (uint32_t i = 0; i < cpu_count; i++) {
        cpu_t *cpu = &cpus[i];
        if (cpu == self || __atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE) != 1)
            continue;
        if ((__atomic_load_n(&cpu->active_cr3, __ATOMIC_ACQUIRE) & PADDR_ENTRY_MASK) != cr3)
            continue;

        __atomic_store_n(&cpu->tlb_flush, 1, __ATOMIC_RELEASE);
        lapic_send_ipi(cpu->lapic_id, SCHED_RESCHED_VECTOR);
        pending |= 1ULL << i;
    }

    // they answer from the ipi, or from bkl_lock if they are stuck waiting on us with irqs off
    while (pending) {
        for (uint32_t i = 0; i < cpu_count; i++) {
            if ((pending & (1ULL << i)) && !__atomic_load_n(&cpus[i].tlb_flush, __ATOMIC_ACQUIRE))
                pending &= ~(1ULL << i);
        }
        asm volatile("pause");
    }
}
