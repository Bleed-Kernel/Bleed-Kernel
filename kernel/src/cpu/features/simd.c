#include <cpu/features/simd.h>
#include <stdint.h>
#include <cpu/cpuid.h>
#include <cpu/control_registers.h>

static inline void xsetbv(uint32_t reg, uint64_t val) {
    __asm__ volatile (
        "xsetbv"
        :: "c"(reg),
           "a"((uint32_t)(val & 0xFFFFFFFF)),
           "d"((uint32_t)(val >> 32))
    );
}

#define CR0_MP          (1UL <<  1)
#define CR0_EM          (1UL <<  2)
#define CR4_OSFXSR      (1UL <<  9)
#define CR4_OSXMMEXCPT  (1UL << 10)
#define CR4_OSXSAVE     (1UL << 18) 

#define XCR0_X87        (1ULL << 0)
#define XCR0_SSE        (1ULL << 1)
#define XCR0_AVX        (1ULL << 2)
#define XCR0_OPMASK     (1ULL << 5)
#define XCR0_ZMM_HI256  (1ULL << 6)
#define XCR0_HI16_ZMM   (1ULL << 7)

#define CPUID1_EDX_SSE    (1u << 25)
#define CPUID1_EDX_SSE2   (1u << 26)
#define CPUID1_ECX_SSE3   (1u <<  0)
#define CPUID1_ECX_SSSE3  (1u <<  9)
#define CPUID1_ECX_SSE4_1 (1u << 19)
#define CPUID1_ECX_SSE4_2 (1u << 20)
#define CPUID1_ECX_XSAVE  (1u << 26)
#define CPUID1_ECX_OSXSAVE (1u << 27)
#define CPUID1_ECX_AVX    (1u << 28)

#define CPUID7_EBX_AVX2   (1u <<  5)
#define CPUID7_EBX_AVX512F (1u << 16)

simd_level_t simd_enable(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    if (max_leaf < 1) return SIMD_NONE;

    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t leaf1_edx = edx;
    uint32_t leaf1_ecx = ecx;

    if (!(leaf1_edx & CPUID1_EDX_SSE)) return SIMD_NONE;

    uint32_t leaf7_ebx = 0;
    if (max_leaf >= 7) {
        cpuid_count(7, 0, &eax, &leaf7_ebx, &ecx, &edx);
    }

    uint64_t cr0 = read_cr0();
    cr0 &= ~CR0_EM;
    cr0 |=  CR0_MP;
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;

    int has_xsave = !!(leaf1_ecx & CPUID1_ECX_XSAVE);
    int has_avx   = !!(leaf1_ecx & CPUID1_ECX_AVX);

    if (has_xsave && has_avx) {
        cr4 |= CR4_OSXSAVE;
    }

    write_cr4(cr4);

    simd_level_t level = SIMD_SSE;

    if (has_xsave && has_avx) {
        uint64_t xcr0 = XCR0_X87 | XCR0_SSE | XCR0_AVX;

        int has_avx512 = !!(leaf7_ebx & CPUID7_EBX_AVX512F);
        if (has_avx512) {
            xcr0 |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        }

        // only ask for what CPUID says XCR0 can hold, anything else is a #GP
        uint32_t xcr0_lo = 0, xcr0_hi = 0;
        cpuid_count(0xD, 0, &xcr0_lo, &ebx, &ecx, &xcr0_hi);
        xcr0 &= ((uint64_t)xcr0_hi << 32) | xcr0_lo;
        has_avx512 = has_avx512 && (xcr0 & XCR0_ZMM_HI256);

        xsetbv(0, xcr0);
        if (has_avx512) {
            level = SIMD_AVX512;
        } else if (leaf7_ebx & CPUID7_EBX_AVX2) {
            level = SIMD_AVX2;
        } else {
            level = SIMD_AVX;
        }
    } else {
        if      (leaf1_ecx & CPUID1_ECX_SSE4_2) level = SIMD_SSE4_2;
        else if (leaf1_ecx & CPUID1_ECX_SSE4_1) level = SIMD_SSE4_1;
        else if (leaf1_ecx & CPUID1_ECX_SSSE3)  level = SIMD_SSSE3;
        else if (leaf1_ecx & CPUID1_ECX_SSE3)   level = SIMD_SSE3;
        else if (leaf1_edx & CPUID1_EDX_SSE2)   level = SIMD_SSE2;
        else                                    level = SIMD_SSE;
    }

    return level;
}

const char *simd_level_name(simd_level_t level) {
    switch (level) {
        case SIMD_NONE:   return "none";
        case SIMD_SSE:    return "SSE";
        case SIMD_SSE2:   return "SSE2";
        case SIMD_SSE3:   return "SSE3";
        case SIMD_SSSE3:  return "SSSE3";
        case SIMD_SSE4_1: return "SSE4.1";
        case SIMD_SSE4_2: return "SSE4.2";
        case SIMD_AVX:    return "AVX";
        case SIMD_AVX2:   return "AVX2";
        case SIMD_AVX512: return "AVX-512F";
        default:          return "unknown";
    }
}