#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

// config header offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION        0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C

/* PCI command register bits */
#define PCI_CMD_IO          0x0001
#define PCI_CMD_MEMORY      0x0002
#define PCI_CMD_BUSMASTER   0x0004

/* Class/subclass for AHCI */
#define PCI_CLASS_STORAGE       0x01
#define PCI_SUBCLASS_SATA       0x06
#define PCI_PROGIF_AHCI         0x01

#define PCI_VENDOR_NONE     0xFFFF

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint32_t bar[6];
} pci_device_t;

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t val);

// scans all PCI bussus
bool pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                     pci_device_t *out);