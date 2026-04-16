#include <drivers/xhci/xhci.h>
#include <drivers/pci/pci.h>
#include <mm/vmm.h>
#include <ACPI/acpi_hpet.h>
#include <stdio.h>
#include <ansii.h>
#include <stdint.h>
#include <drivers/serial/serial.h>

#define XHCI_RESET_TIMEOUT_MS       500
#define XHCI_CNR_TIMEOUT_MS         500  //time we are willing to wait for the controller
#define XHCI_PORT_RESET_TIMEOUT_MS  500  // per port reset poll

// fun fact, XHCI requires a 32 bit aligned mmio for read and writes for registers <= 32 bits,
// for 64 bit registers like crcr and dcbapp write low then high bits.
static inline uint32_t xhci_read32(uintptr_t base, uint32_t offset){
    return *(volatile uint32_t *)(base + offset);
}

static inline void xhci_write32(uintptr_t base, uint32_t offset, uint32_t value){
    *(volatile uint32_t *)(base + offset) = value;
}

static inline uint8_t xhci_read8(uintptr_t base, uint32_t offset){
    return *(volatile uint8_t *)(base + offset);
}

// we need to wait until all bits in the mask are clear in a 32 bit register
static int poll_clear(uintptr_t base, uint32_t offset, uint32_t mask, uint32_t timeout_ms){
    for(uint32_t i = 0; i < timeout_ms; i++){
        if (!(xhci_read32(base, offset) & mask))
            return 0;
        wait_ms(1);
    }
    return -1; // timeout
}

static int poll_set(uintptr_t base, uint32_t offset, uint32_t mask, uint32_t timeout_ms){
    for(uint32_t i = 0; i < timeout_ms; i++){
        if ((xhci_read32(base, offset) & mask) == mask)
            return 0;
        wait_ms(1);
    }
    return -1; // timeout
}

// find map controller

static int xhci_pci_find(uintptr_t *bar0_phys){
    pci_device_t pci_device;

    if (!pci_find_device(PCI_CLASS_SERIAL, PCI_SUBCLASS_USB, 0x30, &pci_device)){
        serial_printf(LOG_ERROR "No XHCI controller found on PCI\n");
        return -1;
    }

    pci_enable_mmio(&pci_device);

    *bar0_phys = (uintptr_t)(pci_device.bar[0] & ~0x0FULL);
    serial_printf(LOG_OK "Found XHCI Controller\n\tVendorID: %lu\n\tDeviceID: %lu\n\tBAR0: 0x%lx\n",
                pci_device.vendor_id, pci_device.device_id, *bar0_phys);

    return 0;
}

// host controller reset
// issue HCRST and wait for it to clear
// we must halt the HC before asserting HCRST
// after HCRST clears we can poll on CNR until the controller is ready

static int xhci_hc_reset(xhci_controller_t *controller){
    uintptr_t op = controller->op_base;

    uint32_t cmd = xhci_read32(op, XHCI_OP_USBCMD);
    cmd &= ~XHCI_CMD_RUN;
    xhci_write32(op, XHCI_OP_USBCMD, cmd);

    if (poll_set(op, XHCI_OP_USBSTS, XHCI_STS_HCH, XHCI_RESET_TIMEOUT_MS)){
        serial_printf(LOG_ERROR "XHCI Timeout, HC refused to HALT\n");
        return -1;
    }

    serial_printf(LOG_OK "XHCI HC halted\n");

    // assert reset

    cmd = xhci_read32(op, XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_HCRST;
    xhci_write32(op, XHCI_OP_USBCMD, cmd);

    if (poll_clear(op, XHCI_OP_USBCMD, XHCI_CMD_HCRST, XHCI_RESET_TIMEOUT_MS)){
        serial_printf(LOG_ERROR "XHCI Timeout, HCRST refused to CLEAR\n");
        return -1;
    }

    if (poll_clear(op, XHCI_OP_USBSTS, XHCI_STS_CNR, XHCI_CNR_TIMEOUT_MS)) {
        serial_printf(LOG_ERROR "XHCI Controller still not ready after reset\n");
        return -1;
    }

    serial_printf(LOG_OK "HC Reset Complete, controller ready!\n");
    return 0;
}

static int xhci_configure(xhci_controller_t *controller){
    uintptr_t op = controller->op_base;

    uint32_t pagesize = xhci_read32(op, XHCI_OP_PAGESIZE);
    if (!(pagesize & 1)){   // nonfatal
        serial_printf(LOG_WARN "Controller doesnt report 4kib page support, (PAGESIZE=0x%x)\n", pagesize);
    }

    uint32_t config = xhci_read32(op, XHCI_OP_CONFIG);
    config &= ~0xFF;
    config |= controller->max_slots & 0xFF;
    xhci_write32(op, XHCI_OP_CONFIG, config);

    // dcbapp must point to a valid 64 byte aligned phsycial page. doenst matter for now

    return 0;
}

static int xhci_start(xhci_controller_t *controller){
    uintptr_t op = controller->op_base;

    uint32_t cmd = xhci_read32(op, XHCI_OP_USBCMD);
    cmd |= XHCI_CMD_RUN;
    cmd &= ~XHCI_CMD_INTE;
    xhci_write32(op, XHCI_OP_USBCMD, cmd);

    if (poll_clear(op, XHCI_OP_USBSTS, XHCI_STS_HCH, 100)) {
        serial_printf(LOG_ERROR "XHCI Controller failed to start as the HCH got stuck\n");
        return -1;
    }

    serial_printf(LOG_OK "XHCI Controller is running\n");
    return 0;
}

void xhci_port_power(xhci_controller_t *controller, uint8_t port, int on){
    uintptr_t op = controller->op_base;
    uint32_t portsc = xhci_read32(op, XHCI_OP_PORTSC(port));

    // So we dont accidentally clear the status flags
    portsc &= ~XHCI_PORTSC_W1C_BITS;

    if (on)
        portsc |= XHCI_PORTSC_PP;
    else
        portsc &= ~XHCI_PORTSC_PP;
    
    xhci_write32(op, XHCI_OP_PORTSC(port), portsc);

    if (on) wait_ms(20);    // we have to wait for power to stable, i love technology.
}

int xhci_port_reset(xhci_controller_t *controller, uint8_t port){
    uintptr_t op = controller->op_base;

    uint32_t portsc = xhci_read32(op, XHCI_OP_PORTSC(port));
    portsc &= ~XHCI_PORTSC_W1C_BITS;
    portsc |= XHCI_PORTSC_PR;
    xhci_write32(op, XHCI_OP_PORTSC(port), portsc);

    if (poll_set(op, XHCI_OP_PORTSC(port), XHCI_PORTSC_PRC,
                 XHCI_PORT_RESET_TIMEOUT_MS)) {
        serial_printf(LOG_INFO "port %u reset timeout\n", port);
        return -1;
    }

    portsc = xhci_read32(op, XHCI_OP_PORTSC(port));
    portsc &= ~XHCI_PORTSC_W1C_BITS;
    portsc |= XHCI_PORTSC_PRC;  // clear
    xhci_write32(op, XHCI_OP_PORTSC(port), portsc);

    return 0;
}

static const char *port_speed_str(uint32_t portsc) {
    switch ((portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT) {
        case 1:  return "Full-Speed (12 Mb/s)";
        case 2:  return "Low-Speed (1.5 Mb/s)";
        case 3:  return "High-Speed (480 Mb/s)";
        case 4:  return "SuperSpeed (5 Gb/s)";
        case 5:  return "SuperSpeed+ (10 Gb/s)";
        default: return "Unknown";
    }
}

int xhci_init(void){
    serial_printf(LOG_INFO "Initialising xhci host controller\n");

    uintptr_t bar0_phys;
    if (xhci_pci_find(&bar0_phys) < 0)
        return -1;

    // map mmio into kernel space.
    // 64kib is always enough for cap + op + port regs

    uintptr_t mmio_virt = vmm_map_mmio(bar0_phys, 0x10000);
    if (!mmio_virt){
        serial_printf(LOG_ERROR "XHCI MMIO Map Failed\n");
        return -1;
    }

    xhci_controller_t controller;
    controller.mmio_base = mmio_virt;
    controller.cap_base = mmio_virt;

    // CAPLENGTH is an 8-bit register at CAP+0x00 
    uint32_t cap_reg = xhci_read32(mmio_virt, 0x00);
    uint8_t caplength = cap_reg & 0xFF;
    uint16_t hciversion = (cap_reg >> 16) & 0xFFFF;

    controller.op_base = mmio_virt + caplength;

    uint32_t hcsp1      = xhci_read32(mmio_virt, XHCI_CAP_HCSPARAMS1);
    controller.max_ports = XHCI_HCSP1_MAX_PORTS(hcsp1);
    controller.max_slots = XHCI_HCSP1_MAX_SLOTS(hcsp1);

    serial_printf(LOG_INFO "XHCI version %x.%02x\n\tcaplength=%u\n\tmax_ports=%u\n\tmax_slots=%u\n",
                    hciversion >> 8, hciversion & 0xFF, caplength, controller.max_ports, controller.max_slots);

    if (xhci_hc_reset(&controller) < 0)
        return -1;

    if (xhci_configure(&controller) < 0)
        return -1;
 
    if (xhci_start(&controller) < 0)
        return -1;

    // power ports
    for (uint8_t i = 0; i < controller.max_ports; i++){
        xhci_port_power(&controller, i, 1);
    }

    wait_ms(200); // the vbus needs time to settle on all the ports before we can probe
    
    uint32_t connected = 0;
    for (uint8_t i = 0; i < controller.max_ports; i++) {
        uint32_t portsc = xhci_read32(controller.op_base, XHCI_OP_PORTSC(i));
 
        if (!(portsc & XHCI_PORTSC_PP)) {
            serial_printf(LOG_INFO "XHCI port %u: no power (controller-managed power?)\n", i);
            continue;
        }
 
        if (!(portsc & XHCI_PORTSC_CCS)) {
            serial_printf(LOG_INFO "XHCI port %u: empty\n", i);
            continue;
        }
 
        serial_printf(LOG_INFO "XHCI port %u: device present, speed=%s - resetting\n",
                i, port_speed_str(portsc));
 
        if (xhci_port_reset(&controller, i) == 0) {
            // Read back PORTSC after reset - speed field is now valid
            portsc = xhci_read32(controller.op_base, XHCI_OP_PORTSC(i));
            serial_printf(LOG_INFO "XHCI port %u: reset done, speed=%s, PED=%d\n",
                    i,
                    port_speed_str(portsc),
                    !!(portsc & XHCI_PORTSC_PED));
            connected++;
        } else {
            serial_printf(LOG_ERROR "XHCI port %u: reset failed. fuck\n", i);
        }
    }
 
    serial_printf(LOG_OK "XHCI init complete and %u device(s) ready for enumeration\n",
            connected);
    return 0;
}