#include <drivers/usb/usb_hcd.h>
#include <drivers/ahci/pci.h>
#include <drivers/serial/serial.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

bool usb_timed_out(uint64_t start, uint64_t spins, uint64_t timeout_ms) {
    uint64_t now = hpet_get_femtoseconds();
    if (now)
        return now - start > timeout_ms * femtosecondsPerMillisecond;
    return spins > timeout_ms * 1000;
}

void usb_delay_ms(uint64_t ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; !usb_timed_out(start, spins, ms); spins++)
        asm volatile("pause");
}

bool usb_wait32(uint64_t base, uint32_t reg, uint32_t mask, bool want_set, uint64_t timeout_ms) {
    uint64_t start = hpet_get_femtoseconds();
    for (uint64_t spins = 0; ; spins++) {
        if (!!(usb_mmio_read32(base, reg) & mask) == want_set)
            return true;
        if (usb_timed_out(start, spins, timeout_ms))
            return false;
        asm volatile("pause");
    }
}

void usb_pci_enable(const pci_device_t *pci_dev) {
    uint16_t cmd = pci_read16(pci_dev->bus, pci_dev->device, pci_dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
    pci_write16(pci_dev->bus, pci_dev->device, pci_dev->function, PCI_COMMAND, cmd);
}

uint64_t usb_bar0_size(const pci_device_t *pci_dev) {
    uint8_t  bus  = pci_dev->bus, dev = pci_dev->device, func = pci_dev->function;
    uint16_t cmd  = pci_read16(bus, dev, func, PCI_COMMAND);
    bool     is64 = (pci_dev->bar[0] & 0x6) == 0x4;

    // decoding off while the BAR holds garbage or it could answer someone elses address
    pci_write16(bus, dev, func, PCI_COMMAND, cmd & ~(PCI_CMD_MEMORY | PCI_CMD_IO));

    pci_write32(bus, dev, func, PCI_BAR0, 0xFFFFFFFF);
    uint64_t mask = pci_read32(bus, dev, func, PCI_BAR0) & ~0xFull;
    pci_write32(bus, dev, func, PCI_BAR0, pci_dev->bar[0]);

    if (is64) {
        pci_write32(bus, dev, func, PCI_BAR1, 0xFFFFFFFF);
        mask |= (uint64_t)pci_read32(bus, dev, func, PCI_BAR1) << 32;
        pci_write32(bus, dev, func, PCI_BAR1, pci_dev->bar[1]);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }

    pci_write16(bus, dev, func, PCI_COMMAND, cmd);
    return mask ? ~mask + 1 : 0;
}

uint64_t usb_map_bar0(const pci_device_t *pci_dev, uint64_t size) {
    uint64_t phys = pci_dev->bar[0] & ~0xFull;
    if ((pci_dev->bar[0] & 0x6) == 0x4)
        phys |= (uint64_t)pci_dev->bar[1] << 32;
    if (phys == 0)
        return 0;

    if (paging_map_mmio(phys, size) < 0)
        return 0;
    return (uint64_t)(uintptr_t)paddr_to_vaddr(phys);
}

void *usb_dma_alloc(size_t pages, paddr_t *phys, bool low_only) {
    paddr_t page = pmm_alloc_pages(pages);
    if (!page)
        return NULL;

    if (low_only && (((uint64_t)page + pages * PAGE_SIZE - 1) >> 32)) {
        serial_printf(LOG_ERROR "usb: dma memory landed above 4G (0x%llx) and this controller cant reach it\n",
                      (uint64_t)page);
        pmm_free_pages(page, pages);
        return NULL;
    }

    void *virt = paddr_to_vaddr(page);
    memset(virt, 0, pages * PAGE_SIZE);
    *phys = page;
    return virt;
}
