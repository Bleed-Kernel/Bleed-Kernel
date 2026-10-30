#pragma once
#include <stdint.h>

typedef enum {
    SIMD_NONE    = 0,
    SIMD_SSE     = 1,
    SIMD_SSE2    = 2,
    SIMD_SSE3    = 3,
    SIMD_SSSE3   = 4,
    SIMD_SSE4_1  = 5,
    SIMD_SSE4_2  = 6,
    SIMD_AVX     = 7,
    SIMD_AVX2    = 8,
    SIMD_AVX512  = 9,
} simd_level_t;

simd_level_t simd_enable(void);
const char  *simd_level_name(simd_level_t level);

// whatever simd_enable settled on
simd_level_t simd_get_level(void);