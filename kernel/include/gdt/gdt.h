#pragma once

#include <stdint.h>
#include <tss/tss.h>

#define KERNEL_CS   0x8
#define KERNEL_SS   0x10
#define USER_CS     (0x18 | 0x3)
#define USER_SS     (0x20 | 0x3)

struct gdt_entry_t{
    uint16_t limit_16;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t limit_8 : 4;
    uint8_t flags   : 4;
    uint8_t base_high;
} __attribute__((packed));

typedef struct GDT{
    struct gdt_entry_t entries[7];
    tss_segment_t tss;
} gdt_t;

struct gdt_ptr_t{
    uint16_t length;
    void* address;
} __attribute__((packed));

struct cpu;

/// @brief initalise the better gdt, replacing the one from LIMINE
void *gdt_init(struct cpu *cpu);
