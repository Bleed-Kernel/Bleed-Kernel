#include <boot/earlyboot_console.h>
#include <panic.h>

static int has_sse42(){
    unsigned int ecx;
    unsigned int eax = 1;
    unsigned int unused;

    asm volatile (
        "cpuid"
        : "=c" (ecx),
          "=a" (unused),
          "=b" (unused),
          "=d" (unused)
        : "a" (eax)
    );

    return (ecx & (1 << 20)) != 0;
}

void enable_sse() {
    if (!has_sse42()){
        EARLY_FAIL("Your System does not have SSE4.2 so cannot continue");
        EARLY_FAIL("You must use a processor with SSE4.2 Capabilities as we use it to make bleed faster");
        EARLY_FAIL("if you are using a virtual machine, make sure KVM is enabled and you are using `-cpu host` for qemu");
        EARLY_FAIL("or set your CPU to a modern processor that is 64 bit");
        asm volatile("cli;hlt");
    }

    float valA = 1.5f;
    float valB = 2.5f;
    float sse_result = 0.0f;

    asm volatile (
        "mov %%cr0, %%rax\n\t"
        "and $~0x4, %%rax\n\t"
        "or  $0x2, %%rax\n\t"
        "mov %%rax, %%cr0\n\t"

        "mov %%cr4, %%rax\n\t"
        "or  %3, %%rax\n\t"      // Bitmask for (1 << 9) | (1 << 10)
        "mov %%rax, %%cr4\n\t"

        "movss %1, %%xmm0\n\t"
        "movss %2, %%xmm1\n\t"
        "addss %%xmm1, %%xmm0\n\t"
        "movss %%xmm0, %0\n\t"
        
        : "=m" (sse_result)
        : "m" (valA),
          "m" (valB),
          "i" ((1 << 9) | (1 << 10))
        : "rax", "xmm0", "xmm1", "memory"
    );

    EARLY_OK("SSE Enabled");
}