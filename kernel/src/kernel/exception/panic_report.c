#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <cpu/percpu.h>
#include <cpu/cpuid.h>
#include <cpu/msrs.h>
#include <cpu/control_registers.h>
#include <cpu/features/simd.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <ACPI/acpi_hpet.h>
#include <ACPI/acpi_time.h>
#include <sched/scheduler.h>
#include <boot/bootloader_interface/generic_bootloader.h>
#include <kernel/exception/panic.h>
#include <kernel/exception/panic_report.h>
#include <kernel/exception/stack_trace.h>

#ifndef BLEED_VERSION
#define BLEED_VERSION   ""
#endif

#define REPORT_MAX      2160
#define SYMBOL_MAX      40
#define MESSAGE_MAX     160
#define CMDLINE_MAX     200
#define NAME_MAX_SHORT  24
#define FRAMES_MAX      16
#define CODE_BEFORE     16
#define CODE_AFTER      16
#define TASKS_MAX       64
#define KERNEL_HALF     0xFFFF800000000000ULL

enum {
    SEC_CAUSE = 1,
    SEC_SYSTEM,
    SEC_CPU,
    SEC_TASK,
    SEC_REGS,
    SEC_CONTROL,
    SEC_MSRS,
    SEC_TRACE,
    SEC_CODE,
    SEC_MEMORY,
    SEC_CPUS,
    SEC_SIMD,
    SEC_TASKS,
};

static uint8_t  blob[REPORT_MAX];
static char     url[sizeof(PANIC_REPORT_URL) + REPORT_MAX * 4 / 3 + 4];
static size_t   len;
static size_t   section_at;
static bool     overflow;
static uint8_t  simd_area[4096] __attribute__((aligned(64)));

// writing

static void put8(uint8_t v) {
    if (len < REPORT_MAX - 4) blob[len++] = v;
    else overflow = true;
}

// zigzag first so higher half addresses come out at 5 bytes and not 10
static void put_num(uint64_t v) {
    uint64_t z = (v << 1) ^ (uint64_t)((int64_t)v >> 63);
    while (z >= 0x80) {
        put8((uint8_t)(z | 0x80));
        z >>= 7;
    }
    put8((uint8_t)z);
}

static void put_str(const char *s, size_t max) {
    size_t n = 0;
    while (s && n < max && s[n]) n++;

    put_num(n);
    for (size_t i = 0; i < n; i++)
        put8((uint8_t)s[i]);
}

static void put_symbol(uint64_t addr) {
    uint64_t sym_addr = 0;
    const char *name = stack_trace_symbol_lookup(addr, &sym_addr);

    put_str(name, SYMBOL_MAX);
    put_num(name ? addr - sym_addr : 0);
}

static void section_begin(uint8_t tag) {
    section_at = len;
    overflow   = false;
    put8(tag);
    put8(0);
    put8(0);
}

// a section that ran out of room is dropped so the ones before it still decode
static void section_end(void) {
    if (overflow) {
        len = section_at;
        return;
    }
    size_t body = len - section_at - 3;
    blob[section_at + 1] = (uint8_t)body;
    blob[section_at + 2] = (uint8_t)(body >> 8);
}

// reading the machine

static bool readable(uint64_t addr, size_t n) {
    if (addr < KERNEL_HALF || addr + n < addr) return false;

    uint64_t cr3 = read_cr3();
    for (uint64_t page = addr & ~0xFFFULL; page < addr + n; page += 0x1000) {
        if (!paging_is_mapped(cr3, page)) return false;
    }
    return true;
}

static uint64_t read_cr8(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr8, %0" : "=r"(v));
    return v;
}

// sections

static void put_cause(struct isr_stackframe *sf, const char *pstring, uint64_t caller) {
    section_begin(SEC_CAUSE);
    put_num(sf ? 0 : 1);
    if (sf) {
        put_num(sf->vector);
        put_num(sf->error_code);
    } else {
        put_str(pstring, MESSAGE_MAX);
        put_num(caller);
        put_symbol(caller);
    }
    section_end();
}

static void put_system(void) {
    section_begin(SEC_SYSTEM);
    put_str(BLEED_VERSION, 48);
    put_str(__VERSION__, 48);
    put_num(hpet_get_femtoseconds() / 1000000000000ULL);
    put_num((uint64_t)clock_now());
    put_str(g_gbi.bootloader_name, NAME_MAX_SHORT);
    put_str(g_gbi.bootloader_version, NAME_MAX_SHORT);
    put_str(g_gbi.cmdline, CMDLINE_MAX);
    put_num(g_gbi.framebuffer.width);
    put_num(g_gbi.framebuffer.height);
    put_num(g_gbi.framebuffer.bpp);
    section_end();
}

static void put_cpu(void) {
    uint32_t a, b, c, d;
    char vendor[13];
    char brand[49];

    cpuid(0, &a, &b, &c, &d);
    uint32_t max_leaf = a;
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;

    memset(brand, 0, sizeof(brand));
    cpuid(0x80000000, &a, &b, &c, &d);
    uint32_t max_ext = a;
    if (max_ext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, &a, &b, &c, &d);
            memcpy(brand + i * 16,      &a, 4);
            memcpy(brand + i * 16 + 4,  &b, 4);
            memcpy(brand + i * 16 + 8,  &c, 4);
            memcpy(brand + i * 16 + 12, &d, 4);
        }
    }

    const char *name = brand;
    while (*name == ' ') name++;

    section_begin(SEC_CPU);
    put_str(vendor, 12);
    put_str(name, 48);

    cpuid(1, &a, &b, &c, &d);
    put_num(a); put_num(b); put_num(c); put_num(d);

    a = b = c = d = 0;
    if (max_leaf >= 7) cpuid_count(7, 0, &a, &b, &c, &d);
    put_num(b); put_num(c); put_num(d);

    a = b = c = d = 0;
    if (max_ext >= 0x80000001) cpuid(0x80000001, &a, &b, &c, &d);
    put_num(c); put_num(d);

    a = b = c = d = 0;
    if (max_ext >= 0x80000008) cpuid(0x80000008, &a, &b, &c, &d);
    put_num(a);

    put_num(this_cpu()->id);
    put_num(this_cpu()->lapic_id);
    put_num(cpu_count);
    section_end();
}

static void put_task(task_t *task) {
    if (!task || !readable((uint64_t)task, sizeof(*task))) return;

    section_begin(SEC_TASK);
    put_num(task->id);
    put_num(task->ppid);
    put_str(task->name, sizeof(task->name));
    put_str(task->exec_path, sizeof(task->exec_path));
    put_num(task->state);
    put_num(task->task_privilege);
    put_num(task->prio_base);
    put_num((uint64_t)(int64_t)task->prio_bonus);
    put_num((uint64_t)task->kernel_stack);
    put_num(task->page_map);
    put_num(task->fs_base);
    put_num(task->sig_pending);
    put_num(task->sig_blocked);
    section_end();
}

static void put_regs(struct isr_stackframe *sf) {
    if (!sf) return;

    uint64_t regs[] = {
        sf->rip, sf->rflags, sf->cs, sf->ss, sf->rsp,
        sf->rax, sf->rbx, sf->rcx, sf->rdx, sf->rsi, sf->rdi, sf->rbp,
        sf->r8, sf->r9, sf->r10, sf->r11, sf->r12, sf->r13, sf->r14, sf->r15,
    };

    section_begin(SEC_REGS);
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        put_num(regs[i]);
    put_symbol(sf->rip);
    section_end();
}

static void put_control(void) {
    uint64_t cr0, cr2, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    cr4 = read_cr4();

    // xgetbv faults unless the os turned xsave on
    uint64_t xcr0 = 0;
    if (cr4 & (1ULL << 18)) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = (uint64_t)hi << 32 | lo;
    }

    section_begin(SEC_CONTROL);
    put_num(cr0);
    put_num(cr2);
    put_num(read_cr3());
    put_num(cr4);
    put_num(read_cr8());
    put_num(rdmsr(0xC0000080));     // efer
    put_num(xcr0);
    section_end();
}

static void put_msrs(void) {
    static const uint32_t msrs[] = {
        MSR_FS_BASE, 0xC0000101, 0xC0000102,                        // fs, gs, kernel gs base
        MSR_IA32_STAR, MSR_IA32_LSTAR, 0xC0000083, MSR_IA32_FMASK,  // syscall
        MSR_PAT, 0x1B,                                              // pat, apic base
    };

    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));

    section_begin(SEC_MSRS);
    for (size_t i = 0; i < sizeof(msrs) / sizeof(msrs[0]); i++)
        put_num(rdmsr(msrs[i]));
    put_num((uint64_t)hi << 32 | lo);
    section_end();
}

static void put_trace(uint64_t rbp) {
    uint64_t frames[FRAMES_MAX];
    int count = stack_trace_collect((uint64_t *)rbp, frames, FRAMES_MAX);

    section_begin(SEC_TRACE);
    put_num(count);
    for (int i = 0; i < count; i++) {
        put_num(frames[i]);
        put_symbol(frames[i]);
    }
    section_end();
}

static void put_code(uint64_t rip) {
    uint64_t start = rip - CODE_BEFORE;
    size_t   count = CODE_BEFORE + CODE_AFTER;

    // the bytes before rip might be on a page that isnt there, keep what is
    if (!readable(start, CODE_BEFORE)) {
        start = rip;
        count = CODE_AFTER;
    }
    if (!readable(start, count)) return;

    section_begin(SEC_CODE);
    put_num(start);
    put_num(rip);
    put_num(count);
    for (size_t i = 0; i < count; i++)
        put8(((uint8_t *)start)[i]);
    section_end();
}

static void put_memory(void) {
    section_begin(SEC_MEMORY);
    put_num(paging_get_usable_mem_size());
    put_num(pmm_available_pages() * 4096);
    put_num(get_max_paddr());
    put_num(g_gbi.kernel_phys_base);
    put_num(g_gbi.kernel_virt_base);
    put_num(g_gbi.hhdm_offset);
    section_end();
}

static void put_cpus(void) {
    section_begin(SEC_CPUS);
    put_num(cpu_count);
    for (uint32_t i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        task_t *current = cpus[i].current;
        bool    ok      = current && readable((uint64_t)current, sizeof(*current));

        put_num(cpus[i].id);
        put_num(cpus[i].lapic_id);
        put_num(cpus[i].online);
        put_num(ok ? current->id : (uint64_t)-1);
        put_str(ok ? current->name : NULL, NAME_MAX_SHORT);
    }
    section_end();
}

// xsave components
#define XS_SSE          1
#define XS_AVX          2
#define XS_ZMM_HI256    6
#define XS_HI16_ZMM     7

static uint64_t simd_in_use;
static uint32_t simd_offset[8];

static uint64_t simd_qword(int i, int q) {
    int comp, at;
    if (i >= 16)    { comp = XS_HI16_ZMM;  at = simd_offset[comp] + (i - 16) * 64 + q * 8; }
    else if (q < 2) { comp = XS_SSE;       at = 160 + i * 16 + q * 8; }
    else if (q < 4) { comp = XS_AVX;       at = simd_offset[comp] + i * 16 + (q - 2) * 8; }
    else            { comp = XS_ZMM_HI256; at = simd_offset[comp] + i * 32 + (q - 4) * 8; }

    // a component xsave left in its init state is all zeros, whatever is in the area
    if (!(simd_in_use & (1ULL << comp)) || at + 8 > (int)sizeof(simd_area)) return 0;
    return *(uint64_t *)(simd_area + at);
}

// the widest vector registers that are switched on
static void put_simd(void) {
    uint64_t cr4 = read_cr4();
    if (!(cr4 & (1ULL << 9))) return;

    __asm__ volatile ("clts");
    memset(simd_area, 0, sizeof(simd_area));

    uint64_t xcr0 = 0;
    uint32_t a, b, c, d;
    if (cr4 & (1ULL << 18)) {
        uint32_t lo, hi;
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = (uint64_t)hi << 32 | lo;

        cpuid_count(0xD, 0, &a, &b, &c, &d);
        if (b > sizeof(simd_area)) xcr0 = 0;
    }

    int width = 16, count = 16;
    if (xcr0) {
        __asm__ volatile ("xsave64 (%0)" :: "r"(simd_area), "a"((uint32_t)xcr0), "d"((uint32_t)(xcr0 >> 32)) : "memory");
        simd_in_use = *(uint64_t *)(simd_area + 512);

        for (int comp = XS_AVX; comp <= XS_HI16_ZMM; comp++) {
            cpuid_count(0xD, comp, &a, &b, &c, &d);
            simd_offset[comp] = b;
        }
        if ((xcr0 & (1ULL << XS_ZMM_HI256)) && (xcr0 & (1ULL << XS_HI16_ZMM))) {
            width = 64;
            count = 32;
        } else if (xcr0 & (1ULL << XS_AVX)) {
            width = 32;
        }
    } else {
        __asm__ volatile ("fxsave64 (%0)" :: "r"(simd_area) : "memory");
        simd_in_use = 1ULL << XS_SSE;
    }

    for (; width >= 16; width /= 2) {
        if (width < 64) count = 16;

        section_begin(SEC_SIMD);
        put_num(simd_get_level());
        put_num(width);
        put_num(count);
        for (int i = 0; i < count; i++) {
            for (int q = 0; q < width / 8; q++)
                put_num(simd_qword(i, q));
        }
        section_end();
        if (!overflow) break;
    }
}

static void put_tasks(void) {
    section_begin(SEC_TASKS);

    size_t count_at = len;
    put8(0);

    int count = 0;
    task_t *task = task_list_head;
    while (task && count < TASKS_MAX && readable((uint64_t)task, sizeof(*task))) {
        put_num(task->id);
        put_num(task->ppid);
        put_num(task->state);
        put_num(task->task_privilege);
        put_num(task->prio_base);
        put_num(task->on_cpu);
        put_str(task->name, NAME_MAX_SHORT);
        count++;

        task = task->next;
        if (task == task_list_head) break;
    }
    if (!overflow) blob[count_at] = (uint8_t)count;
    section_end();
}

// encoding
static uint32_t crc32(const uint8_t *data, size_t n) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static void base64url(char *out, const uint8_t *in, size_t n) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];

        *out++ = digits[v >> 18 & 63];
        *out++ = digits[v >> 12 & 63];
        if (i + 1 < n) *out++ = digits[v >> 6 & 63];
        if (i + 2 < n) *out++ = digits[v & 63];
    }
    *out = 0;
}

const char *panic_report_url(struct isr_stackframe *sf, const char *pstring, uint64_t rbp, uint64_t caller) {
    len = 0;
    put8('B');
    put8(PANIC_REPORT_FORMAT);

    put_cause(sf, pstring, caller);
    put_system();
    put_cpu();
    put_task(get_current_task());
    put_regs(sf);
    put_control();
    put_msrs();
    put_trace(rbp);
    if (sf) put_code(sf->rip);
    put_memory();
    put_cpus();
    put_simd();
    put_tasks();

    uint32_t crc = crc32(blob, len);
    for (int i = 0; i < 4; i++)
        blob[len++] = (uint8_t)(crc >> (i * 8));

    memcpy(url, PANIC_REPORT_URL, sizeof(PANIC_REPORT_URL) - 1);
    base64url(url + sizeof(PANIC_REPORT_URL) - 1, blob, len);
    return url;
}
