#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mm/pmm.h>
#include <drivers/usb/usb.h>

/*
    The documentation for the the macros here are only sparcely commented
    so if you need something more complete please start here
    https://github.com/spotify/linux/blob/master/drivers/usb/host/ohci.h
*/

#define OHCI_MAX_CONTROLLERS    4
#define OHCI_MAX_PORTS          15
#define OHCI_MAX_DEVICES        8
#define OHCI_MAX_PIPES          8   // interrupt endpoints per controller

// operational register offsets
#define OHCI_REG_REVISION       0x00
#define OHCI_REG_CONTROL        0x04
#define OHCI_REG_CMDSTATUS      0x08
#define OHCI_REG_INT_STATUS     0x0C
#define OHCI_REG_INT_ENABLE     0x10
#define OHCI_REG_INT_DISABLE    0x14
#define OHCI_REG_HCCA           0x18
#define OHCI_REG_CONTROL_HEAD   0x20
#define OHCI_REG_CONTROL_CUR    0x24
#define OHCI_REG_BULK_HEAD      0x28
#define OHCI_REG_BULK_CUR       0x2C
#define OHCI_REG_DONE_HEAD      0x30
#define OHCI_REG_FM_INTERVAL    0x34
#define OHCI_REG_PERIODIC_START 0x40
#define OHCI_REG_RH_DESC_A      0x48
#define OHCI_REG_RH_STATUS      0x50
#define OHCI_REG_RH_PORT        0x54    // + 4 per port

// HcControl
#define OHCI_CTRL_CBSR_4_1      (3u << 0)   // control:bulk service ratio
#define OHCI_CTRL_PLE           (1u << 2)   // periodic list enable
#define OHCI_CTRL_CLE           (1u << 4)   // control list enable
#define OHCI_CTRL_HCFS_MASK     (3u << 6)
#define OHCI_CTRL_HCFS_RESET    (0u << 6)
#define OHCI_CTRL_HCFS_OPER     (2u << 6)
#define OHCI_CTRL_IR            (1u << 8)   // interrupts routed to SMM cause the firmware owns it
#define OHCI_CTRL_RWC           (1u << 9)

// HcCommandStatus
#define OHCI_CMD_HCR            (1u << 0)   // host controller reset
#define OHCI_CMD_CLF            (1u << 1)   // control list filled
#define OHCI_CMD_OCR            (1u << 3)   // ownership change request

// HcInterruptStatus / Enable / Disable
#define OHCI_INT_WDH            (1u << 1)   // writeback done head
#define OHCI_INT_OC             (1u << 30)  // ownership change
#define OHCI_INT_MIE            (1u << 31)  // master enable

// HcFmInterval
#define OHCI_FM_INTERVAL_MASK   0x3FFF
#define OHCI_FM_INTERVAL_DEFAULT 0x2EDF     // 11999 bit times, 1ms frame
#define OHCI_FM_FIT             (1u << 31)

// HcRhDescriptorA
#define OHCI_RHA_NDP_MASK       0xFF        // number of downstream ports
#define OHCI_RHA_POTPGT_SHIFT   24          // power on to power good, units of 2ms

// HcRhStatus
#define OHCI_RHS_LPSC           (1u << 16)  // write = set global power

// HcRhPortStatus, reads are status and writes are commands
#define OHCI_PORT_CCS           (1u << 0)   // device connected
#define OHCI_PORT_PES           (1u << 1)   // port enabled      / write = enable
#define OHCI_PORT_PRS           (1u << 4)   // reset in progress / write = start reset
#define OHCI_PORT_PPS           (1u << 8)   // port powered      / write = power on
#define OHCI_PORT_LSDA          (1u << 9)   // low speed device attached
#define OHCI_PORT_CSC           (1u << 16)  // connect status change
#define OHCI_PORT_PRSC          (1u << 20)  // reset finished
#define OHCI_PORT_CHANGE_MASK   0x001F0000  // write 1s to clear

// endpoint descriptor info word
#define OHCI_ED_FA(a)           ((uint32_t)(a) & 0x7F)
#define OHCI_ED_EN(n)           (((uint32_t)(n) & 0xF) << 7)
#define OHCI_ED_DIR_TD          (0u << 11)  // direction comes from each TD
#define OHCI_ED_DIR_IN          (2u << 11)
#define OHCI_ED_LOWSPEED        (1u << 13)
#define OHCI_ED_SKIP            (1u << 14)
#define OHCI_ED_MPS(n)          (((uint32_t)(n) & 0x7FF) << 16)

#define OHCI_ED_HEAD_HALTED     (1u << 0)
#define OHCI_ED_HEAD_CARRY      (1u << 1)   // data toggle carried between TDs
#define OHCI_PTR_MASK           0xFFFFFFF0u

// transfer descriptor info word
#define OHCI_TD_ROUNDING        (1u << 18)  // short packets are fine
#define OHCI_TD_DP_SETUP        (0u << 19)
#define OHCI_TD_DP_OUT          (1u << 19)
#define OHCI_TD_DP_IN           (2u << 19)
#define OHCI_TD_DI_NOW          (0u << 21)  // report it on the very next frame
#define OHCI_TD_DI_NONE         (7u << 21)
#define OHCI_TD_TOGGLE_ED       (0u << 24)  // toggle carried in the ED
#define OHCI_TD_TOGGLE_DATA0    (2u << 24)
#define OHCI_TD_TOGGLE_DATA1    (3u << 24)
#define OHCI_TD_CC_SHIFT        28
#define OHCI_TD_CC_NOT_ACCESSED (0xFu << 28)

#define OHCI_CC_NOERROR         0x0
#define OHCI_CC_STALL           0x4

#define OHCI_RESET_TIMEOUT_MS   10
#define OHCI_HANDOFF_TIMEOUT_MS 1000
#define OHCI_PORT_RESET_MS      100
#define OHCI_CTRL_TIMEOUT_MS    500
#define OHCI_PIPE_MAX_ERRORS    8       // strikes before we decide a device is gone

#define OHCI_POLL_FRAMES        8       // interrupt endpoints get serviced every 8ms

// everything in here is touched by the controller behind our back, hence the volatile
typedef struct ohci_ed {
    volatile uint32_t info;
    volatile uint32_t tail;
    volatile uint32_t head;
    volatile uint32_t next;
} __attribute__((aligned(16))) ohci_ed_t;

typedef struct ohci_td {
    volatile uint32_t info;
    volatile uint32_t cbp;      // current buffer pointer, 0 once its all moved
    volatile uint32_t next;
    volatile uint32_t be;       // address of the last byte in the buffer
} __attribute__((aligned(16))) ohci_td_t;

typedef struct ohci_hcca {
    volatile uint32_t int_table[32];
    volatile uint16_t frame_number;
    volatile uint16_t pad;
    volatile uint32_t done_head;
    uint8_t           reserved[116];
} __attribute__((aligned(256))) ohci_hcca_t;

// td pool layout, the control endpoint owns the first 4 and each pipe gets a pair
#define OHCI_TD_SETUP           0
#define OHCI_TD_DATA            1
#define OHCI_TD_STATUS          2
#define OHCI_TD_CTRL_TAIL       3
#define OHCI_TD_PIPE_BASE       4
#define OHCI_TD_COUNT           (OHCI_TD_PIPE_BASE + OHCI_MAX_PIPES * 2)

// ed pool layout
#define OHCI_ED_CONTROL         0
#define OHCI_ED_PIPE_BASE       1
#define OHCI_ED_COUNT           (OHCI_ED_PIPE_BASE + OHCI_MAX_PIPES)

#define OHCI_PIPE_BUF_SIZE      64

// the controller only speaks 32bit physical addresses so every structure it
// reads lives together in one low page
typedef struct ohci_dma {
    ohci_hcca_t hcca;                           // must be 256-byte aligned
    ohci_ed_t   eds[OHCI_ED_COUNT];             // 16-byte aligned
    ohci_td_t   tds[OHCI_TD_COUNT];             // 16-byte aligned
    uint8_t     setup[16];
    uint8_t     pipe_buf[OHCI_MAX_PIPES][OHCI_PIPE_BUF_SIZE];
    uint8_t     ctrl_buf[USB_MAX_CONFIG_LEN];
} ohci_dma_t;

typedef struct ohci_pipe {
    bool               used;
    ohci_ed_t         *ed;
    ohci_td_t         *td;      // the one the controller is working on
    ohci_td_t         *tail;    // spare, swaps with td every report
    uint8_t           *buf;
    uint16_t           len;
    uint8_t            errors;
    usb_interrupt_cb_t cb;
    void              *ctx;
} ohci_pipe_t;

typedef struct ohci_hc {
    bool         present;
    uint64_t     base;          // vaddr of the OHCI MMIO base
    paddr_t      dma_phys;
    ohci_dma_t  *dma;
    uint8_t      port_count;
    uint8_t      next_address;

    uint32_t     ctrl_retired;  // bitmask of control TDs the controller has handed back
    uint32_t     ctrl_cc;       // first bad condition code seen, 0 if clean

    ohci_pipe_t  pipes[OHCI_MAX_PIPES];
    usb_device_t devices[OHCI_MAX_DEVICES];
    int          device_count;
} ohci_hc_t;

// Find every OHCI controller, take it off the firmware and enumerate the root ports
void ohci_init(void);

// Run a control transfer on endpoint 0 and wait for it
// returns the number of data bytes moved or negative on failure
int ohci_control(usb_device_t *dev, const usb_setup_t *setup, void *data);

// Start polling an interrupt IN endpoint, cb fires from ohci_poll() for every report
int ohci_interrupt_in(usb_device_t *dev, uint8_t endpoint, uint16_t max_packet,
                      usb_interrupt_cb_t cb, void *ctx);

// Collect finished transfers, called off the timer tick since we dont route the PCI interrupt
void ohci_poll(void);
