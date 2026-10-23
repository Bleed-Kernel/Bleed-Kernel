#include <drivers/usb/ehci.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/ahci/pci.h>
#include <drivers/serial/serial.h>
#include <mm/paging.h>
#include <ACPI/acpi_hpet.h>
#include <ansii.h>
#include <stdio.h>

// a usb2 controller sits in front of the ohci ones
void ehci_release_ports(void) {
    pci_device_t pci_dev;
    for (int n = 0; pci_find_device_nth(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                                        PCI_PROGIF_EHCI, n, &pci_dev); n++) {
        usb_pci_enable(&pci_dev);

        uint64_t base = usb_map_bar0(&pci_dev, PAGE_SIZE_4K);
        if (!base) {
            serial_printf(LOG_WARN "ehci: " PCI_SPEC " has no usable BAR\n", PCI_SPEC_ARGS(&pci_dev));
            continue;
        }

        // the legacy support capability lives in pci config space, ask the BIOS for ownership
        uint8_t eecp = (uint8_t)(usb_mmio_read32(base, EHCI_CAP_HCCPARAMS) >> 8);
        for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
            uint32_t cap = pci_read32(pci_dev.bus, pci_dev.device, pci_dev.function, eecp);
            if ((cap & 0xFF) == EHCI_LEGSUP_CAPID) {
                pci_write32(pci_dev.bus, pci_dev.device, pci_dev.function,
                            eecp, cap | EHCI_LEGSUP_OS);

                uint64_t start = hpet_get_femtoseconds();
                for (uint64_t spins = 0; ; spins++) {
                    cap = pci_read32(pci_dev.bus, pci_dev.device, pci_dev.function, eecp);
                    if (!(cap & EHCI_LEGSUP_BIOS))
                        break;
                    if (usb_timed_out(start, spins, EHCI_HANDOFF_TIMEOUT_MS)) {
                        serial_printf(LOG_WARN "ehci: BIOS wont hand over, taking it anyway\n");
                        break;
                    }
                    asm volatile("pause");
                }
                // no more SMIs
                pci_write32(pci_dev.bus, pci_dev.device, pci_dev.function, eecp + 4, 0);
            }
            eecp = (uint8_t)(cap >> 8);
        }

        uint64_t op = base + (usb_mmio_read32(base, EHCI_CAP_LENGTH) & 0xFF);

        // it has to be halted before it will accept a reset
        usb_mmio_write32(op, EHCI_OP_USBINTR, 0);
        usb_mmio_write32(op, EHCI_OP_USBCMD, usb_mmio_read32(op, EHCI_OP_USBCMD) & ~EHCI_CMD_RUN);
        usb_wait32(op, EHCI_OP_USBSTS, EHCI_STS_HALTED, true, 50);

        usb_mmio_write32(op, EHCI_OP_USBCMD, EHCI_CMD_RESET);
        if (!usb_wait32(op, EHCI_OP_USBCMD, EHCI_CMD_RESET, false, 250))
            serial_printf(LOG_WARN "ehci: reset timeout\n");

        // route everything to the companion controllers
        usb_mmio_write32(op, EHCI_OP_CONFIGFLAG, 0);

        serial_printf(LOG_INFO "ehci: " PCI_SPEC " ports released to companions\n",
                      PCI_SPEC_ARGS(&pci_dev));
    }
}
