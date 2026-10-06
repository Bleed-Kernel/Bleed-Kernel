#pragma once
#include <stdint.h>

typedef struct {
    uint32_t reserved;
    uint64_t rsp0;
    uint32_t reserved2[22];
    uint16_t reserved3;
    uint16_t iopb;          // offset of the io permission bitmap from the tss base
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t limit_flags;
    uint8_t base_high;
    uint32_t base_high2;
    uint32_t _reserved;
} __attribute__((packed)) tss_segment_t;

extern tss_t tss;

void tss_init(void);