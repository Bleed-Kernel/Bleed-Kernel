#include <cpu/smp.h>
#include <cpu/percpu.h>
#include <cpu/msrs.h>
#include <cpu/control_registers.h>
#include <cpu/features/features.h>
#include <vendor/limine_bootloader/limine.h>
#include <sched/scheduler.h>
#include <idt/idt.h>
#include <mm/paging.h>
#include <drivers/serial/serial.h>
#include <ACPI/acpi_hpet.h>
#include <ACPI/acpi_apic.h>
#include <ansii.h>

#define AP_START_TIMEOUT_MS     1000
#define CR0_TS                  (1ULL << 3)
#define CR4_OSXSAVE             (1ULL << 18)

__attribute__((used, section(".requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
};

// what the bsp ended up with after boot
typedef struct {
    uint64_t cr0, cr4, efer, xcr0, pat;
    uint64_t star, lstar, fmask;
} boot_regs_t;

static boot_regs_t boot_regs;

static inline uint64_t xgetbv(uint32_t reg) {
    uint32_t lo, hi;
    asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(reg));
    return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv(uint32_t reg, uint64_t val) {
    asm volatile("xsetbv" :: "c"(reg), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void boot_regs_save(void) {
    boot_regs.cr0   = read_cr0() & ~CR0_TS;
    boot_regs.cr4   = read_cr4();
    boot_regs.efer  = rdmsr(MSR_EFER);
    boot_regs.xcr0  = (boot_regs.cr4 & CR4_OSXSAVE) ? xgetbv(0) : 0;
    boot_regs.pat   = rdmsr(MSR_PAT);
    boot_regs.star  = rdmsr(MSR_IA32_STAR);
    boot_regs.lstar = rdmsr(MSR_IA32_LSTAR);
    boot_regs.fmask = rdmsr(MSR_IA32_FMASK);
}

static void boot_regs_load(void) {
    wrmsr(MSR_EFER, boot_regs.efer);
    write_cr0(boot_regs.cr0);
    write_cr4(boot_regs.cr4);
    if (boot_regs.cr4 & CR4_OSXSAVE)
        xsetbv(0, boot_regs.xcr0);
    wrmsr(MSR_PAT, boot_regs.pat);
    wrmsr(MSR_IA32_STAR, boot_regs.star);
    wrmsr(MSR_IA32_LSTAR, boot_regs.lstar);
    wrmsr(MSR_IA32_FMASK, boot_regs.fmask);

    // pat changed, nothing cached under the old one can stay
    paging_switch_address_space(kernel_page_map);
}

__attribute__((noreturn))
static void ap_idle(cpu_t *cpu) {
    task_t *idle = cpu->idle_task;
    idle->run_tsc   = rdtsc();
    cpu->switch_tsc = idle->run_tsc;
    cpu_set_current(idle);

    // without a tick it would never look at the run queue
    int ticking = lapic_ap_init() == 0;
    __atomic_store_n(&cpu->online, ticking ? 1 : 2, __ATOMIC_RELEASE);
    if (!ticking) {
        for (;;)
            asm volatile("cli; hlt");
    }

    for (;;)
        sched_idle();
}

// limine calls this on the ap
__attribute__((noreturn))
static void ap_entry(struct limine_mp_info *info) {
    cpu_t *cpu = (cpu_t *)info->extra_argument;

    boot_regs_load();
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    gdt_init(cpu);
    tss_init(cpu);
    idt_load();

    uint64_t stack_top = ((uint64_t)cpu->idle_task->kernel_stack + KERNEL_STACK_SIZE) & ~0xFULL;
    cpu->tss.rsp0   = stack_top;
    cpu->kernel_rsp = stack_top;

    // off limines stack
    asm volatile(
        "mov %0, %%rsp\n\t"
        "xor %%rbp, %%rbp\n\t"
        "call *%1"
        :: "r"(stack_top), "r"(ap_idle), "D"(cpu)
        : "memory"
    );
    __builtin_unreachable();
}

static int ap_wait_online(cpu_t *cpu) {
    uint64_t deadline = sched_deadline_ms(AP_START_TIMEOUT_MS);
    while (!__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) {
        if (sched_deadline_passed(deadline))
            return -1;
        asm volatile("pause");
    }
    return 0;
}

void smp_init(void) {
    struct limine_mp_response *mp = mp_request.response;
    if (!mp) {
        serial_printf(LOG_WARN "smp: limine gave no mp response, running on the bsp only\n");
        return;
    }

    this_cpu()->lapic_id = mp->bsp_lapic_id;
    serial_printf(LOG_INFO "smp: %llu cpus, bsp lapic %u\n", mp->cpu_count, mp->bsp_lapic_id);

    boot_regs_save();

    for (uint64_t i = 0; i < mp->cpu_count; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id)
            continue;
        if (cpu_count >= MAX_CPUS) {
            serial_printf(LOG_WARN "smp: more than %u cpus, the rest stay parked in limine\n", MAX_CPUS);
            break;
        }

        cpu_t *cpu    = &cpus[cpu_count];
        cpu->self     = cpu;
        cpu->id       = cpu_count;
        cpu->lapic_id = info->lapic_id;
        if (!sched_create_idle_task(cpu)) {
            serial_printf(LOG_ERROR "smp: no memory for cpu %u's idle task\n", cpu->id);
            break;
        }

        info->extra_argument = (uint64_t)cpu;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);

        // a cpu that never shows up might still later
        if (ap_wait_online(cpu) != 0) {
            serial_printf(LOG_ERROR "smp: cpu %u (lapic %u) didnt come up\n", cpu->id, cpu->lapic_id);
            break;
        }
        if (cpu->online != 1) {
            serial_printf(LOG_ERROR "smp: cpu %u (lapic %u) has no timer, left parked\n", cpu->id, cpu->lapic_id);
            break;
        }

        serial_printf(LOG_OK "smp: cpu %u (lapic %u) online\n", cpu->id, cpu->lapic_id);
        __atomic_store_n(&cpu_count, cpu_count + 1, __ATOMIC_RELEASE);
    }
}
