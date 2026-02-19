#pragma once

#include <stdint.h>

typedef struct {
    uint16_t    offset16;   // lower 0-15
    uint16_t    selector;   // the segment selector for CS
    uint8_t     ist;        // interupt stack table offset
    uint8_t     attributes; // also holds the type 
    uint16_t    offset32;   // higher 16-31
    uint32_t    offset64;   // higher 32-63
    uint32_t    zero;       // that top reserved bit
} __attribute__((packed)) idt_entry_t;

/// @brief idt pointer
typedef struct {
    uint16_t    limit;
    uint64_t    address;
} __attribute__((packed)) idt_ptr_t;

void idt_init();