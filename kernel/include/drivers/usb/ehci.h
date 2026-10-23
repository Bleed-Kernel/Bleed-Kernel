#pragma once

// EHCI bits
#define EHCI_CAP_LENGTH     0x00
#define EHCI_CAP_HCCPARAMS  0x08
#define EHCI_OP_USBCMD      0x00
#define EHCI_OP_USBSTS      0x04
#define EHCI_OP_USBINTR     0x08
#define EHCI_OP_CONFIGFLAG  0x40

#define EHCI_CMD_RUN        (1u << 0)
#define EHCI_CMD_RESET      (1u << 1)
#define EHCI_STS_HALTED     (1u << 12)

#define EHCI_LEGSUP_CAPID   0x01
#define EHCI_LEGSUP_BIOS    (1u << 16)
#define EHCI_LEGSUP_OS      (1u << 24)

#define EHCI_HANDOFF_TIMEOUT_MS 1000

// Take every EHCI controller off the firmware
void ehci_release_ports(void);
