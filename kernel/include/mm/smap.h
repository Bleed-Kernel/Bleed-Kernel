#pragma once

#define CPUID_LEAF_BASIC_MAX     0
#define CPUID_LEAF_EXT_FEATURES  7

#define CPUID_7_EBX_SMEP (1u << 7)
#define CPUID_7_EBX_SMAP (1u << 20)

#define CPU_CR4_SMEP_BIT (1ULL << 20)
#define CPU_CR4_SMAP_BIT (1ULL << 21)

#define SMAP_MISSING    1
#define SMEP_MISSING    2
#define BOTH_MISSING    3
#define FULLY_SUPPORTED 0

// never return, break or goto out of the block, clac only runs when it falls off the end
#define SMAP_ALLOW for (int _i = (stac(), 0); !_i; clac(), _i++)

int SMAP_init(void);

extern int smap_supported;

static inline void stac(void){
    if (!smap_supported) return;
    asm volatile("stac" ::: "cc");
}

static inline void clac(void){
    if (!smap_supported) return;
    asm volatile("clac" ::: "cc");
}