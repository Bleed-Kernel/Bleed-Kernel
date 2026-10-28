#pragma once
#include <stdint.h>

#define MSR_FS_BASE     0xC0000100
#define MSR_PAT         0x277
#define MSR_IA32_STAR   0xC0000081
#define MSR_IA32_LSTAR  0xC0000082
#define MSR_IA32_FMASK  0xC0000084

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = val & 0xFFFFFFFF;
    uint32_t hi = val >> 32;
    asm volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}