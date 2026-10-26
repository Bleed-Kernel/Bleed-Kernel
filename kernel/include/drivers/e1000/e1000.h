#pragma once

#include <stdint.h>
#include <stdbool.h>

// intel 8254x
// this is what qemu will give the machine
// by default so im running with the e1000
#define E1000_DEV_82540EM       0x100E
#define E1000_DEV_82545EM       0x100F
#define E1000_DEV_82543GC       0x1004
#define E1000_DEV_82545GM       0x1026

#define PCI_CLASS_NETWORK       0x02
#define PCI_SUBCLASS_ETHERNET   0x00

// registers
#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EERD      0x0014
#define E1000_ICR       0x00C0
#define E1000_IMS       0x00D0
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_TIPG      0x0410
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_MTA       0x5200
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404

#define E1000_CTRL_ASDE     (1u << 5)
#define E1000_CTRL_SLU      (1u << 6)
#define E1000_CTRL_ILOS     (1u << 7)
#define E1000_CTRL_RST      (1u << 26)
#define E1000_CTRL_PHY_RST  (1u << 31)
#define E1000_CTRL_LRST     (1u << 3)

#define E1000_STATUS_LU     (1u << 1)

#define E1000_EERD_START    (1u << 0)
#define E1000_EERD_DONE     (1u << 4)

#define E1000_RAH_AV        (1u << 31)

#define E1000_RCTL_EN       (1u << 1)
#define E1000_RCTL_BAM      (1u << 15)    // take broadcasts, dhcp and arp need them
#define E1000_RCTL_BSIZE_2K (0u << 16)
#define E1000_RCTL_SECRC    (1u << 26)    // strip the crc so lengths are just the frame

#define E1000_TCTL_EN       (1u << 1)
#define E1000_TCTL_PSP      (1u << 3)
#define E1000_TCTL_CT       (0x10u << 4)
#define E1000_TCTL_COLD     (0x40u << 12)

#define E1000_TIPG_DEFAULT  (10u | (10u << 10) | (10u << 20))

#define E1000_RXD_DD        (1u << 0)
#define E1000_RXD_EOP       (1u << 1)

#define E1000_TXD_CMD_EOP   (1u << 0)
#define E1000_TXD_CMD_IFCS  (1u << 1)
#define E1000_TXD_CMD_RS    (1u << 3)
#define E1000_TXD_DD        (1u << 0)

#define E1000_RX_DESCS      64      // ring bytes have to be a multiple of 128
#define E1000_TX_DESCS      64
#define E1000_BUF_SIZE      2048
#define E1000_RESET_MS      10

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

// find the first e1000 and register it as a net interface
void e1000_init(void);

// called off the timer tick, kicks the net service when a frame landed
void e1000_poll(void);
