#include <stdint.h>
#include <cpu/msrs.h>
#include <cpu/features/features.h>
#include <gdt/gdt.h>
#include <syscalls/syscall.h>

#define MSR_IA32_STAR   0xC0000081
#define MSR_IA32_LSTAR  0xC0000082
#define MSR_IA32_FMASK  0xC0000084
#define EFER_SCE        (1ULL << 0)

extern void syscall_entry(void);

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    uint64_t star = ((uint64_t)USER_CS << 48) | ((uint64_t)KERNEL_CS << 32);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);

    // Clear IF on entry to avoid interrupts before we switch to the kernel stack.
    wrmsr(MSR_IA32_FMASK, (1ULL << 9));
}
