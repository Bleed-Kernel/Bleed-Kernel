#pragma once

#include <stdint.h>
#include <stdbool.h>

static inline uint32_t mmio_read32(uint64_t base, uint32_t offset) {
    return *(volatile uint32_t *)(uintptr_t)(base + offset);
}

static inline void mmio_write32(uint64_t base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + offset) = val;
}

// 64bit registers go low half first, some hardware latches on the high write
static inline uint64_t mmio_read64(uint64_t base, uint32_t offset) {
    uint64_t lo = mmio_read32(base, offset);
    uint64_t hi = mmio_read32(base, offset + 4);
    return lo | (hi << 32);
}

static inline void mmio_write64(uint64_t base, uint32_t offset, uint64_t val) {
    mmio_write32(base, offset,     (uint32_t)val);
    mmio_write32(base, offset + 4, (uint32_t)(val >> 32));
}

// spin until (reg & mask) is set or clear, false if timeout_ms went by first
bool mmio_wait32(uint64_t base, uint32_t reg, uint32_t mask, bool want_set, uint64_t timeout_ms);
