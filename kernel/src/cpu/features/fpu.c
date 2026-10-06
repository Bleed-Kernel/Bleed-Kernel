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
 * Lazy FPU switching. A context switch never touches the FPU registers, it only sets CR0.TS
 * when the task coming in isnt the one whose state is sitting in them. The first FPU, SSE or
 * AVX instruction that task runs traps with #NM and thats where the save and restore happen,
 * so a task that never uses them costs nothing. The kernel is built without SSE so nothing
 * in here can trip the trap by accident.
 *
 * XSAVE is used when the cpu has it, the area is sized by CPUID for whatever XCR0 enables so
 * turning on more state later (AVX-512 and friends) needs no changes here.
 */

#define CR0_TS              (1ULL << 3)
#define CPUID1_ECX_OSXSAVE  (1u << 27)

#define FXSAVE_AREA_SIZE    512
#define FPU_FCW_OFFSET      0
#define FPU_MXCSR_OFFSET    24
#define FPU_FCW_DEFAULT     0x037F      // all x87 exceptions masked, 64 bit precision
#define FPU_MXCSR_DEFAULT   0x1F80      // all sse exceptions masked

static size_t   fpu_area_size  = FXSAVE_AREA_SIZE;
static size_t   fpu_area_pages = 1;
static int      fpu_use_xsave  = 0;
static uint8_t *fpu_clean      = NULL;  // what a brand new task starts from
static task_t  *fpu_owner      = NULL;  // whose state is in the registers right now

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

    // dont leave the owner pointing at a task thats about to be freed
    unsigned long flags = irq_push();
    if (fpu_owner == task)
        fpu_owner = NULL;
    irq_restore(flags);

    vmm_free_pages(task->fpu_state, fpu_area_pages);
    task->fpu_state = NULL;
}

void fpu_task_reset(task_t *task) {
    unsigned long flags = irq_push();

    // whatever is in the registers belongs to the old image, disown it so the next use reloads
    if (fpu_owner == task)
        fpu_owner = NULL;
    memcpy(task->fpu_state, fpu_clean, fpu_area_size);
    if (task == get_current_task())
        cr0_set_bits(CR0_TS);

    irq_restore(flags);
}

void fpu_task_copy(task_t *dst, task_t *src) {
    unsigned long flags = irq_push();

    // the saved copy is stale while src owns the registers, bring it up to date first
    if (fpu_owner == src) {
        uint64_t cr0 = read_cr0();
        clts();
        fpu_save(src->fpu_state);
        write_cr0(cr0);
    }
    memcpy(dst->fpu_state, src->fpu_state, fpu_area_size);

    irq_restore(flags);
}

void fpu_switch(task_t *next) {
    if (next == fpu_owner)
        clts();
    else
        cr0_set_bits(CR0_TS);
}

int fpu_handle_nm(void) {
    task_t *current = get_current_task();
    if (!current || !current->fpu_state)
        return 0;

    clts();
    if (fpu_owner == current)
        return 1;

    // the last task to use the fpu has been switched out with its state still in the registers
    if (fpu_owner)
        fpu_save(fpu_owner->fpu_state);

    fpu_restore(current->fpu_state);
    fpu_owner = current;
    return 1;
}
