#include <stdint.h>
#include <cpu/msrs.h>
#include <cpu/features/features.h>
#include <gdt/gdt.h>
#include <syscalls/syscall.h>

#define MSR_IA32_STAR   0xC0000081
#define MSR_IA32_LSTAR  0xC0000082
#define MSR_IA32_FMASK  0xC0000084
#define EFER_SCE        (1ULL << 0)

#define RFLAGS_TF        (1ULL << 8)
#define RFLAGS_IF        (1ULL << 9)
#define RFLAGS_DF        (1ULL << 10)
#define RFLAGS_AC        (1ULL << 18)

extern void syscall_entry(void);

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    uint64_t star = ((uint64_t)USER_CS << 48) | ((uint64_t)KERNEL_CS << 32);
    wrmsr(MSR_IA32_STAR, star);
    wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry);

    // IF so nothing interrupts us before we are on the kernel stack, DF because the C side
    // assumes its clear, TF so a single stepped task cant trap inside the kernel and AC so
    // userspace cant walk in with SMAP already switched off
    wrmsr(MSR_IA32_FMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_AC);
}
