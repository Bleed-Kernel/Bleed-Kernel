#include <drivers/ahci/pci.h>
#include <cpu/io.h>
#include <stdbool.h>

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

// seek busses
bool pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     pci_device_t *out) {
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