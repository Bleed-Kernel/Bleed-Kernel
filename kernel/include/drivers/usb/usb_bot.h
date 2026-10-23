#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <drivers/usb/usb_storage.h>

// bulk only transport
// https://www.usb.org/sites/default/files/usbmassbulk_10.pdf

#define USB_BOT_CBW_SIGNATURE   0x43425355  // "USBC"
#define USB_BOT_CSW_SIGNATURE   0x53425355  // "USBS"
#define USB_BOT_CBW_IN          0x80

#define USB_BOT_CBW_OFFSET      0
#define USB_BOT_CSW_OFFSET      64

// CSW status
#define USB_BOT_PASSED          0
#define USB_BOT_FAILED          1
#define USB_BOT_PHASE_ERROR     2

typedef struct __attribute__((packed)) usb_bot_cbw {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cdb_length;
    uint8_t  cdb[16];
} usb_bot_cbw_t;

typedef struct __attribute__((packed)) usb_bot_csw {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} usb_bot_csw_t;

// Run one SCSI command, data goes through ms->bounce.
int usb_bot_command(usb_storage_t *ms, const uint8_t *cdb, uint8_t cdb_len,
                    bool in, uint32_t len, uint32_t *residue);

// Bulk only mass storage reset and unhalt both pipes
int usb_bot_reset(usb_storage_t *ms);

// Highest LUN the device has
uint8_t usb_bot_max_lun(usb_storage_t *ms);
