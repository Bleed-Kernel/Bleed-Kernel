#include <pci/pci.h>
#include <cpu/io.h>
#include <stdbool.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <mm/paging.h>
#include <ansii.h>

// config space build
static inline uint32_t pci_addr(uint8_t bus, uint8_t dev,
                                 uint8_t func, uint8_t offset) {
    return (1u << 31)
         | ((uint32_t)bus    << 16)
         | ((uint32_t)dev    << 11)
         | ((uint32_t)func   <<  8)
         | (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, dev, func, offset & ~3u);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t val = pci_read32(bus, dev, func, offset & ~3u);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func,
                 uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func,
                 uint8_t offset, uint16_t val) {

    uint32_t cur = pci_read32(bus, dev, func, offset & ~3u); // should help us avoid clobbering
    int shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_write32(bus, dev, func, offset & ~3u, cur);
}

// seek busses, index picks which match you get back
bool pci_find_device_nth(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                         int index, pci_device_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            
            // gets skipped if theres no device dont worry
            uint16_t vendor = pci_read16(bus, dev, 0, PCI_VENDOR_ID);
            if (vendor == PCI_VENDOR_NONE) continue;

            uint8_t hdr_type = pci_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (hdr_type & 0x80) ? 8 : 1; /* multi-function? */

            for (uint8_t func = 0; func < max_func; func++) {
                vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vendor == PCI_VENDOR_NONE) continue;

                uint8_t cls  = pci_read8(bus, dev, func, PCI_CLASS);
                uint8_t sub  = pci_read8(bus, dev, func, PCI_SUBCLASS);
                uint8_t pif  = pci_read8(bus, dev, func, PCI_PROG_IF);

                if (cls != class_code || sub != subclass || pif != prog_if)
                    continue;
                if (index-- > 0)
                    continue;

                out->bus       = (uint8_t)bus;
                out->device    = dev;
                out->function  = func;
                out->vendor_id = vendor;
                out->device_id = pci_read16(bus, dev, func, PCI_DEVICE_ID);
                out->class_code = cls;
                out->subclass   = sub;
                out->prog_if    = pif;

                for (int i = 0; i < 6; i++)
                    out->bar[i] = pci_read32(bus, dev, func,
                                             PCI_BAR0 + i * 4);

                return true;
            }
        }
    }
    return false;
}

bool pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     pci_device_t *out) {
    return pci_find_device_nth(class_code, subclass, prog_if, 0, out);
}

static pci_device_t *pci_list  = NULL;
static size_t        pci_total = 0;

void pci_scan(void) {
    size_t cap = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            if (pci_read16(bus, dev, 0, PCI_VENDOR_ID) == PCI_VENDOR_NONE)
                continue;

            uint8_t max_func = (pci_read8(bus, dev, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < max_func; func++) {
                uint16_t vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vendor == PCI_VENDOR_NONE)
                    continue;

                if (pci_total == cap) {
                    size_t        bigger = cap ? cap * 2 : 32;
                    pci_device_t *list   = kmalloc(bigger * sizeof(pci_device_t));
                    if (!list)
                        return;
                    if (pci_list) {
                        memcpy(list, pci_list, pci_total * sizeof(pci_device_t));
                        kfree(pci_list);
                    }
                    pci_list = list;
                    cap      = bigger;
                }

                pci_device_t *p = &pci_list[pci_total++];
                p->bus        = (uint8_t)bus;
                p->device     = dev;
                p->function   = func;
                p->vendor_id  = vendor;
                p->device_id  = pci_read16(bus, dev, func, PCI_DEVICE_ID);
                p->class_code = pci_read8(bus, dev, func, PCI_CLASS);
                p->subclass   = pci_read8(bus, dev, func, PCI_SUBCLASS);
                p->prog_if    = pci_read8(bus, dev, func, PCI_PROG_IF);
                for (int i = 0; i < 6; i++)
                    p->bar[i] = pci_read32(bus, dev, func, PCI_BAR0 + i * 4);

                serial_printf(LOG_INFO "pci: " PCI_SPEC " %04x:%04x class %02x:%02x:%02x\n", PCI_SPEC_ARGS(p),
                              p->vendor_id, p->device_id, p->class_code, p->subclass, p->prog_if);
            }
        }
    }
}

size_t pci_count(void) {
    return pci_total;
}

pci_device_t *pci_get(size_t index) {
    return index < pci_total ? &pci_list[index] : NULL;
}

void pci_enable(const pci_device_t *dev) {
    uint16_t cmd = pci_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
    pci_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

uint64_t pci_bar_size(const pci_device_t *dev, int bar) {
    uint8_t  bus  = dev->bus, slot = dev->device, func = dev->function;
    uint8_t  reg  = PCI_BAR0 + bar * 4;
    uint16_t cmd  = pci_read16(bus, slot, func, PCI_COMMAND);
    bool     is64 = bar < 5 && (dev->bar[bar] & 0x7) == 0x4;

    // decoding off while the BAR holds garbage or it could answer someone elses address
    pci_write16(bus, slot, func, PCI_COMMAND, cmd & ~(PCI_CMD_MEMORY | PCI_CMD_IO));

    pci_write32(bus, slot, func, reg, 0xFFFFFFFF);
    uint64_t mask = pci_read32(bus, slot, func, reg) & ~0xFull;
    pci_write32(bus, slot, func, reg, dev->bar[bar]);

    if (is64) {
        pci_write32(bus, slot, func, reg + 4, 0xFFFFFFFF);
        mask |= (uint64_t)pci_read32(bus, slot, func, reg + 4) << 32;
        pci_write32(bus, slot, func, reg + 4, dev->bar[bar + 1]);
    } else {
        mask |= 0xFFFFFFFF00000000ull;
    }

    pci_write16(bus, slot, func, PCI_COMMAND, cmd);
    return mask ? ~mask + 1 : 0;
}

uint64_t pci_map_bar(const pci_device_t *dev, int bar, uint64_t size) {
    uint64_t phys = dev->bar[bar] & ~0xFull;
    if (bar < 5 && (dev->bar[bar] & 0x6) == 0x4)
        phys |= (uint64_t)dev->bar[bar + 1] << 32;
    if (phys == 0)
        return 0;

    if (paging_map_mmio(phys, size) < 0)
        return 0;
    return (uint64_t)(uintptr_t)paddr_to_vaddr(phys);
}
