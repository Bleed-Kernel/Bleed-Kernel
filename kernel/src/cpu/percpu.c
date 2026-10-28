#include <cpu/percpu.h>
#include <cpu/msrs.h>
#include <cpu/cpuid.h>

cpu_t    cpus[MAX_CPUS];
uint32_t cpu_count = 1;

void cpu_bsp_early_init(void) {
    cpu_t *cpu = &cpus[0];
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);

    cpu->self     = cpu;
    cpu->id       = 0;
    cpu->lapic_id = ebx >> 24;

    // kernel side always runs with GS_BASE on its cpu_t
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}
