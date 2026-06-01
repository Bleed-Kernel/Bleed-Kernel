#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>
#include <stdbool.h>

struct isr_stackframe {
    uint64_t r15; uint64_t r14; uint64_t r13; uint64_t r12;
    uint64_t r11; uint64_t r10; uint64_t r9;  uint64_t r8;
    uint64_t rbp; uint64_t rdi; uint64_t rsi; uint64_t rdx;
    uint64_t rcx; uint64_t rbx; uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

const char* exception_name(uint8_t vector);

/// @brief Raise an exception and halt the system
/// @param reason the kernel can choose a reason to relay to the user
__attribute__((noreturn))
void ke_panic(struct isr_stackframe *sf, const char *pstring);

#endif
