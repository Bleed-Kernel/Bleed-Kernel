#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <drivers/ahci/pci.h>

// bits every host controller driver needs, the mmio, the timing and the pci setup
static inline uint32_t usb_mmio_read32(uint64_t base, uint32_t offset) {
    return *(volatile uint32_t *)(uintptr_t)(base + offset);
}

static inline void usb_mmio_write32(uint64_t base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + offset) = val;
}

// 64bit registers go low half first, some controllers latch on the high write
static inline uint64_t usb_mmio_read64(uint64_t base, uint32_t offset) {
    uint64_t lo = usb_mmio_read32(base, offset);
    uint64_t hi = usb_mmio_read32(base, offset + 4);
    return lo | (hi << 32);
}

static inline void usb_mmio_write64(uint64_t base, uint32_t offset, uint64_t val) {
    usb_mmio_write32(base, offset,     (uint32_t)val);
    usb_mmio_write32(base, offset + 4, (uint32_t)(val >> 32));
}

// has timeout_ms passed since start, spins is the fallback when there is no HPET
bool usb_timed_out(uint64_t start, uint64_t spins, uint64_t timeout_ms);

// DO NOT TRY TO YEILD IN THIS FUNCTION
void usb_delay_ms(uint64_t ms);

// wait for (reg & mask) to become set or clear
bool usb_wait32(uint64_t base, uint32_t reg, uint32_t mask, bool want_set, uint64_t timeout_ms);

// Turn on memory decoding and bus mastering
void usb_pci_enable(const pci_device_t *pci_dev);

// Size BAR0 by writing all ones at it, has to happen before decoding is turned on
uint64_t usb_bar0_size(const pci_device_t *pci_dev);

// Map BAR0 into the HHDM and return its vaddr, 0 if it wont
uint64_t usb_map_bar0(const pci_device_t *pci_dev, uint64_t size);

// Zeroed contiguous pages for a controller to dma into, low_only keeps them under 4G.
void *usb_dma_alloc(size_t pages, paddr_t *phys, bool low_only);

#define PCI_SPEC "%02x:%02x.%x"
#define PCI_SPEC_ARGS(p) (p)->bus, (p)->device, (p)->function
