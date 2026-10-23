#pragma once

#include <stdint.h>

/*
    everything here is from the xHCI 1.2 spec, section numbers next to the
    bigger blocks so you know where to look when something doesnt add up
    https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html
*/

// flip to 1 to log every TRB and event, way too loud to leave on
#define XHCI_TRACE              0

// capability registers (5.3)
#define XHCI_CAP_CAPLENGTH      0x00    // 8 bits, op regs start this far in
#define XHCI_CAP_HCIVERSION     0x02    // 16 bits
#define XHCI_CAP_HCSPARAMS1     0x04
#define XHCI_CAP_HCSPARAMS2     0x08
#define XHCI_CAP_HCCPARAMS1     0x10
#define XHCI_CAP_DBOFF          0x14
#define XHCI_CAP_RTSOFF         0x18

#define XHCI_HCS1_MAX_SLOTS(p)  ((p) & 0xFF)
#define XHCI_HCS1_MAX_INTRS(p)  (((p) >> 8) & 0x7FF)
#define XHCI_HCS1_MAX_PORTS(p)  (((p) >> 24) & 0xFF)

#define XHCI_HCS2_SCRATCH(p)    ((((p) >> 21) & 0x1F) << 5 | (((p) >> 27) & 0x1F))

#define XHCI_HCC1_AC64          (1u << 0)
#define XHCI_HCC1_CSZ           (1u << 2)
#define XHCI_HCC1_PPC           (1u << 3)
#define XHCI_HCC1_XECP(p)       (((p) >> 16) & 0xFFFF)

// operational registers (5.4)
#define XHCI_OP_USBCMD          0x00
#define XHCI_OP_USBSTS          0x04
#define XHCI_OP_PAGESIZE        0x08
#define XHCI_OP_DNCTRL          0x14
#define XHCI_OP_CRCR            0x18
#define XHCI_OP_DCBAAP          0x30
#define XHCI_OP_CONFIG          0x38
#define XHCI_OP_PORTSC(n)       (0x400 + 0x10 * ((n) - 1))

#define XHCI_CMD_RUN            (1u << 0)
#define XHCI_CMD_HCRST          (1u << 1)
#define XHCI_CMD_INTE           (1u << 2)

#define XHCI_STS_HCH            (1u << 0) 
#define XHCI_STS_HSE            (1u << 2)   // host system error, a dma went somewhere bad:/
#define XHCI_STS_EINT           (1u << 3)
#define XHCI_STS_PCD            (1u << 4)
#define XHCI_STS_CNR            (1u << 11)  // controller not ready
#define XHCI_STS_HCE            (1u << 12)  // host controller error, its given up

#define XHCI_CRCR_RCS           (1u << 0)
#define XHCI_CRCR_CA            (1u << 2)   // command abort
#define XHCI_CRCR_CRR           (1u << 3)   // command ring running

// PORTSC (5.4.8), reads are status and the change bits are write 1 to clear
#define XHCI_PORTSC_CCS         (1u << 0)   // something is plugged in
#define XHCI_PORTSC_PED         (1u << 1)   // enabled, writing a 1 here DISABLES the port
#define XHCI_PORTSC_OCA         (1u << 3)
#define XHCI_PORTSC_PR          (1u << 4)   // usb2 style reset
#define XHCI_PORTSC_PLS(p)      (((p) >> 5) & 0xF)
#define XHCI_PORTSC_PP          (1u << 9)
#define XHCI_PORTSC_SPEED(p)    (((p) >> 10) & 0xF)
#define XHCI_PORTSC_PIC_MASK    (3u << 14)
#define XHCI_PORTSC_CSC         (1u << 17)
#define XHCI_PORTSC_PEC         (1u << 18)
#define XHCI_PORTSC_WRC         (1u << 19)
#define XHCI_PORTSC_OCC         (1u << 20)
#define XHCI_PORTSC_PRC         (1u << 21)
#define XHCI_PORTSC_PLC         (1u << 22)
#define XHCI_PORTSC_CEC         (1u << 23)
#define XHCI_PORTSC_WCE         (1u << 25)
#define XHCI_PORTSC_WDE         (1u << 26)
#define XHCI_PORTSC_WOE         (1u << 27)
#define XHCI_PORTSC_WPR         (1u << 31)  // usb3 warm reset

#define XHCI_PORTSC_CHANGE_MASK (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
                                 XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                                 XHCI_PORTSC_CEC)
// the only bits that are safe to write back as they were read
#define XHCI_PORTSC_KEEP        (XHCI_PORTSC_PP | XHCI_PORTSC_PIC_MASK | \
                                 XHCI_PORTSC_WCE | XHCI_PORTSC_WDE | XHCI_PORTSC_WOE)

#define XHCI_PLS_U0             0
#define XHCI_PLS_RX_DETECT      5
#define XHCI_PLS_POLLING        7

// PORTSC speed ids with the default speed table (7.2.2.1.1)
#define XHCI_SPEED_FULL         1
#define XHCI_SPEED_LOW          2
#define XHCI_SPEED_HIGH         3
#define XHCI_SPEED_SUPER        4
#define XHCI_SPEED_SUPER_PLUS   5

// runtime registers (5.5), only interrupter 0 is used
#define XHCI_RT_IR0             0x20
#define XHCI_IR_IMAN            0x00
#define XHCI_IR_IMOD            0x04
#define XHCI_IR_ERSTSZ          0x08
#define XHCI_IR_ERSTBA          0x10
#define XHCI_IR_ERDP            0x18

#define XHCI_IMAN_IP            (1u << 0)
#define XHCI_ERDP_EHB           (1u << 3)

// extended capabilities (7)
#define XHCI_XCAP_ID(c)         ((c) & 0xFF)
#define XHCI_XCAP_NEXT(c)       (((c) >> 8) & 0xFF)
#define XHCI_XCAP_LEGACY        1
#define XHCI_XCAP_PROTOCOL      2

#define XHCI_LEGSUP_BIOS        (1u << 16)
#define XHCI_LEGSUP_OS          (1u << 24)
// keep the reserved bits, clear the smi enables, ack the smi status bits. same as linux
#define XHCI_LEGCTL_KEEP        ((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))
#define XHCI_LEGCTL_SMI_EVENTS  (0x7u << 29)

#define XHCI_PROTO_MAJOR(c)     (((c) >> 24) & 0xFF)
#define XHCI_PROTO_MINOR(c)     (((c) >> 16) & 0xFF)
#define XHCI_PROTO_PORT_OFF(c)  ((c) & 0xFF)
#define XHCI_PROTO_PORT_CNT(c)  (((c) >> 8) & 0xFF)

// intel chipsets that have ehci next to xhci share ports between them
#define XHCI_INTEL_XUSB2PR      0xD0
#define XHCI_INTEL_XUSB2PRM     0xD4
#define XHCI_INTEL_USB3_PSSEN   0xD8
#define XHCI_INTEL_USB3PRM      0xDC

// TRBs (6.4)
#define XHCI_TRB_CYCLE          (1u << 0)
#define XHCI_TRB_TC             (1u << 1)   // link trbs only, flip the cycle when following it
#define XHCI_TRB_ISP            (1u << 2)   // raise an event on a short packet
#define XHCI_TRB_CH             (1u << 4)   // chained onto the next trb, same TD
#define XHCI_TRB_IOC            (1u << 5)
#define XHCI_TRB_IDT            (1u << 6)   // the data is in the parameter field itself
#define XHCI_TRB_BSR            (1u << 9)
#define XHCI_TRB_DIR_IN         (1u << 16)  // data and status stages
#define XHCI_TRB_TYPE(t)        ((uint32_t)(t) << 10)
#define XHCI_TRB_GET_TYPE(c)    (((c) >> 10) & 0x3F)
#define XHCI_TRB_SLOT(s)        ((uint32_t)(s) << 24)
#define XHCI_TRB_GET_SLOT(c)    (((c) >> 24) & 0xFF)
#define XHCI_TRB_EP(e)          ((uint32_t)(e) << 16)
#define XHCI_TRB_GET_EP(c)      (((c) >> 16) & 0x1F)

#define XHCI_TRB_TRT_NONE       (0u << 16)
#define XHCI_TRB_TRT_OUT        (2u << 16)
#define XHCI_TRB_TRT_IN         (3u << 16)

#define XHCI_TRB_LEN(l)         ((uint32_t)(l) & 0x1FFFF)
#define XHCI_TRB_TD_SIZE(n)     ((uint32_t)((n) > 31 ? 31 : (n)) << 17)
#define XHCI_EVT_CC(s)          (((s) >> 24) & 0xFF)
#define XHCI_EVT_RESIDUAL(s)    ((s) & 0xFFFFFF)
#define XHCI_EVT_PORT(p)        (((p) >> 24) & 0xFF)

// trb types
#define XHCI_TRB_NORMAL         1
#define XHCI_TRB_SETUP          2
#define XHCI_TRB_DATA           3
#define XHCI_TRB_STATUS         4
#define XHCI_TRB_LINK           6
#define XHCI_TRB_ENABLE_SLOT    9
#define XHCI_TRB_DISABLE_SLOT   10
#define XHCI_TRB_ADDRESS_DEVICE 11
#define XHCI_TRB_CONFIGURE_EP   12
#define XHCI_TRB_EVALUATE_CTX   13
#define XHCI_TRB_RESET_EP       14
#define XHCI_TRB_STOP_EP        15
#define XHCI_TRB_SET_TR_DEQUEUE 16
#define XHCI_TRB_NOOP_CMD       23
#define XHCI_TRB_EVT_TRANSFER   32
#define XHCI_TRB_EVT_COMMAND    33
#define XHCI_TRB_EVT_PORT       34
#define XHCI_TRB_EVT_HOST       37

// completion codes (6.4.5), the ones we act on, xhci_cc_name knows the rest
#define XHCI_CC_INVALID         0       // also what we report for a timeout
#define XHCI_CC_SUCCESS         1
#define XHCI_CC_DATA_BUFFER     2
#define XHCI_CC_BABBLE          3
#define XHCI_CC_TRANSACTION     4
#define XHCI_CC_TRB             5
#define XHCI_CC_STALL           6
#define XHCI_CC_SHORT_PACKET    13
#define XHCI_CC_STOPPED         26
#define XHCI_CC_STOPPED_LENGTH  27

// slot context (6.2.2)
#define XHCI_SLOT_SPEED(s)          ((uint32_t)(s) << 20)
#define XHCI_SLOT_ENTRIES(n)        ((uint32_t)(n) << 27)
#define XHCI_SLOT_ENTRIES_MASK      (0x1Fu << 27)
#define XHCI_SLOT_ROOT_PORT(p)      ((uint32_t)(p) << 16)
#define XHCI_SLOT_GET_ADDRESS(d3)   ((d3) & 0xFF)

// endpoint context (6.2.3)
#define XHCI_EP_GET_STATE(d0)       ((d0) & 0x7)
#define XHCI_EP_INTERVAL(i)         ((uint32_t)(i) << 16)
#define XHCI_EP_ESIT_HI(n)          ((uint32_t)((n) >> 16) << 24)
#define XHCI_EP_CERR(n)             ((uint32_t)(n) << 1)
#define XHCI_EP_TYPE(t)             ((uint32_t)(t) << 3)
#define XHCI_EP_BURST(b)            ((uint32_t)(b) << 8)
#define XHCI_EP_MPS(m)              ((uint32_t)(m) << 16)
#define XHCI_EP_AVG_TRB(n)          ((uint32_t)(n) & 0xFFFF)
#define XHCI_EP_ESIT_LO(n)          ((uint32_t)((n) & 0xFFFF) << 16)

#define XHCI_EP_STATE_DISABLED  0
#define XHCI_EP_STATE_RUNNING   1
#define XHCI_EP_STATE_HALTED    2
#define XHCI_EP_STATE_STOPPED   3
#define XHCI_EP_STATE_ERROR     4

#define XHCI_EP_TYPE_BULK_OUT   2
#define XHCI_EP_TYPE_INT_OUT    3
#define XHCI_EP_TYPE_CONTROL    4
#define XHCI_EP_TYPE_BULK_IN    6
#define XHCI_EP_TYPE_INT_IN     7

typedef struct xhci_trb {
    volatile uint64_t param;
    volatile uint32_t status;
    volatile uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

typedef struct xhci_erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;
