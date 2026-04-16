#pragma once
#include <stdint.h>
#include <stddef.h>
 
//capabilty registers
#define XHCI_CAP_CAPLENGTH      0x00
#define XHCI_CAP_HCIVERSION     0x02
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCSPARAMS3     0x0C
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18
 
// operational registers
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_CONFIG          0x38
// port register block
#define XHCI_OP_PORTSC(n)       (0x400 + ((n) * 0x10))
 
// usb command bits
#define XHCI_CMD_RUN            (1u << 0)   // stop start
#define XHCI_CMD_HCRST          (1u << 1)   // HC enable
#define XHCI_CMD_INTE           (1u << 2)   // interrupt enable
 
// usb status bits
#define XHCI_STS_HCH            (1u << 0)   // HC Halted
#define XHCI_STS_HSE            (1u << 2)   // host system error
#define XHCI_STS_CNR            (1u << 11)  // not ready
#define XHCI_STS_HCE            (1u << 12)  // HC Error
 
// portsc bits
#define XHCI_PORTSC_CCS         (1u << 0)   // connection status
#define XHCI_PORTSC_PED         (1u << 1)   // port enabled
#define XHCI_PORTSC_PR          (1u << 4)   // port reset
#define XHCI_PORTSC_PP          (1u << 9)   // port power
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK  (0xFu << 10)
 
// Write-1-to-clear status bits must be masked out on RMW to avoid accidentally clearing them when writing other fields
#define XHCI_PORTSC_W1C_BITS    (0xFEu << 16)   // CSC,PEC,WRC,OCC,PRC,PLC,CEC
#define XHCI_PORTSC_PRC         (1u << 21)      // Port Reset Change (W1C)
#define XHCI_PORTSC_CSC         (1u << 17)      // Connect Status Change (W1C)
 
// HCSPARAMS1 extract
#define XHCI_HCSP1_MAX_SLOTS(x) (((x) >>  0) & 0xFF)
#define XHCI_HCSP1_MAX_PORTS(x) (((x) >> 24) & 0xFF)
 
// PCI identifiers (incomplete)
#define PCI_CLASS_SERIAL        0x0C
#define PCI_SUBCLASS_USB        0x03
#define PCI_PROG_IF_XHCI        0x30
 
// driver state
typedef struct {
    uintptr_t   mmio_base;  // mmio base from bar
    uintptr_t   cap_base;   // mmio_base
    uintptr_t   op_base;    // == mmio_base + CAPLENGTH
    uint8_t     max_ports;
    uint8_t     max_slots;
} xhci_controller_t;

int  xhci_init(void);
void xhci_port_power(xhci_controller_t *ctrl, uint8_t port, int on);
int  xhci_port_reset(xhci_controller_t *ctrl, uint8_t port);