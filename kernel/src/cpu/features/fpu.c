#include <cpu/features/fpu.h>
#include <cpu/control_registers.h>
#include <cpu/cpuid.h>
#include <sched/scheduler.h>
#include <kernel/exception/panic.h>
#include <drivers/serial/serial.h>
#include <mm/vmm.h>
#include <mm/spinlock.h>
#include <string.h>
#include <ansii.h>

/*
 * Eager FPU switching. A user task's FPU, SSE and AVX state is saved when it is switched out
 * and loaded when it is switched in, so it can come back on any cpu. Kernel tasks skip both,
 * This could be a problem because the kernel does actually use SSE in the mem functions but I havent noticed any issues
 * the second i do ill patch this
 *
 * XSAVE is used when the cpu has it, the area is sized by CPUID for whatever XCR0 enables so
 * turning on more state later (AVX-512 and friends) needs no changes here.
 */
#define CR0_TS              (1ULL << 3)
#define CPUID1_ECX_OSXSAVE  (1u << 27)

#define FXSAVE_AREA_SIZE    512
#define FPU_FCW_OFFSET      0
#define FPU_MXCSR_OFFSET    24
#define FPU_FCW_DEFAULT     0x037F      // all x87 exceptions masked
#define FPU_MXCSR_DEFAULT   0x1F80      // all sse exceptions masked

static size_t   fpu_area_size  = FXSAVE_AREA_SIZE;
static size_t   fpu_area_pages = 1;
static int      fpu_use_xsave  = 0;
static uint8_t *fpu_clean      = NULL;  // what a brand new task starts from

static inline void clts(void) {
    asm volatile("clts");
}

static inline void fpu_save(uint8_t *area) {
    if (fpu_use_xsave)
        asm volatile("xsave64 (%0)" :: "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
    else
        asm volatile("fxsave64 (%0)" :: "r"(area) : "memory");
}

static inline void fpu_restore(uint8_t *area) {
    if (fpu_use_xsave)
        asm volatile("xrstor64 (%0)" :: "r"(area), "a"(0xFFFFFFFFu), "d"(0xFFFFFFFFu) : "memory");
    else
        asm volatile("fxrstor64 (%0)" :: "r"(area) : "memory");
}

void fpu_init(void) {
    uint32_t eax, ebx, ecx, edx;

    // OSXSAVE mirrors CR4, its only set if simd_enable turned xsave on
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (ecx & CPUID1_ECX_OSXSAVE) {
        cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);
        fpu_area_size = ebx;    // size for exactly what XCR0 has enabled
        fpu_use_xsave = 1;
    }
    fpu_area_pages = (fpu_area_size + PAGE_SIZE - 1) / PAGE_SIZE;

    // whole pages because xsave wants 64 byte alignment and kmalloc doesnt promise that
    fpu_clean = vmm_alloc_pages(fpu_area_pages);
    if (!fpu_clean)
        ke_panic(NULL, "Failed to allocate the clean FPU state");

    // a zeroed xsave header tells xrstor to reset every component. MXCSR is the exception,
    // its always loaded from the area and 0 would unmask every sse exception
    *(uint16_t *)(fpu_clean + FPU_FCW_OFFSET)   = FPU_FCW_DEFAULT;
    *(uint32_t *)(fpu_clean + FPU_MXCSR_OFFSET) = FPU_MXCSR_DEFAULT;

    clts();
    serial_printf(LOG_OK "FPU: lazy %s, %zu byte save area\n",
                  fpu_use_xsave ? "xsave" : "fxsave", fpu_area_size);
}

int fpu_task_init(task_t *task) {
    task->fpu_state = vmm_alloc_pages(fpu_area_pages);
    if (!task->fpu_state)
        return -1;

    memcpy(task->fpu_state, fpu_clean, fpu_area_size);
    return 0;
}

void fpu_task_free(task_t *task) {
    if (!task->fpu_state) return;

    vmm_free_pages(task->fpu_state, fpu_area_pages);
    task->fpu_state = NULL;
}

// only user tasks have anything in the registers worth keeping, the kernel never touches them
static int fpu_user(task_t *task) {
    return task && task->task_privilege == P_USER && task->fpu_state;
}

void fpu_task_reset(task_t *task) {
    unsigned long flags = irq_push();

    // the registers still hold the old image, they would get saved over the clean copy
    memcpy(task->fpu_state, fpu_clean, fpu_area_size);
    if (task == get_current_task())
        fpu_restore(task->fpu_state);

    irq_restore(flags);
}

void fpu_task_copy(task_t *dst, task_t *src) {
    unsigned long flags = irq_push();

    // the saved copy is stale while src is running, bring it up to date first
    if (src == get_current_task() && fpu_user(src))
        fpu_save(src->fpu_state);
    memcpy(dst->fpu_state, src->fpu_state, fpu_area_size);

    irq_restore(flags);
}

void fpu_switch(task_t *prev, task_t *next) {
    if (fpu_user(prev))
        fpu_save(prev->fpu_state);
    if (fpu_user(next))
        fpu_restore(next->fpu_state);
}

int fpu_handle_nm(void) {
    // TS is never set any more, this is only here for a firmware or ap that left it on
    clts();
    return 1;
}
