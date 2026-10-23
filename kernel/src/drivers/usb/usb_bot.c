#include <drivers/usb/usb_bot.h>
#include <drivers/usb/usb.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

int usb_bot_reset(usb_storage_t *ms) {
    serial_printf(LOG_WARN "usbstorage%d: reset recovery\n", ms->index);

    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_INTERFACE,
        .request      = USB_MSC_REQ_RESET,
        .index        = ms->interface,
    };
    int r = usb_control(ms->dev, &setup, NULL);
    if (r < 0)
        serial_printf(LOG_WARN "usbstorage%d: mass storage reset failed (%d)\n", ms->index, r);

    // the reset leaves both pipes halted on the device side (5.3.4)
    usb_clear_halt(ms->dev, ms->ep_in);
    usb_clear_halt(ms->dev, ms->ep_out);
    return r;
}

uint8_t usb_bot_max_lun(usb_storage_t *ms) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_INTERFACE,
        .request      = USB_MSC_REQ_GET_MAX_LUN,
        .index        = ms->interface,
        .length       = 1,
    };
    uint8_t lun = 0;

    // single lun devices are allowed to stall this one
    int r = usb_control(ms->dev, &setup, &lun);
    if (r < 1) {
        serial_printf(LOG_INFO "usbstorage%d: GET_MAX_LUN not answered (%d), assuming one lun\n",
                      ms->index, r);
        return 0;
    }
    return lun;
}

// a stalled CSW read is allowed once, after that the device is confused (6.7.2)
static int usb_bot_read_csw(usb_storage_t *ms, usb_bot_csw_t *csw) {
    uint8_t *buf = ms->cmd + USB_BOT_CSW_OFFSET;

    for (int attempt = 0; attempt < 2; attempt++) {
        memset(buf, 0, sizeof(*csw));
        int r = usb_bulk(ms->dev, ms->ep_in, buf, sizeof(*csw));
        if (r == USB_ERR_STALL) {
            serial_printf(LOG_WARN "usbstorage%d: CSW stalled, clearing and trying again\n", ms->index);
            usb_clear_halt(ms->dev, ms->ep_in);
            continue;
        }
        if (r != (int)sizeof(*csw)) {
            serial_printf(LOG_WARN "usbstorage%d: CSW read gave %d bytes\n", ms->index, r);
            return USB_ERR;
        }

        memcpy(csw, buf, sizeof(*csw));
        return 0;
    }
    return USB_ERR;
}

int usb_bot_command(usb_storage_t *ms, const uint8_t *cdb, uint8_t cdb_len,
                    bool in, uint32_t len, uint32_t *residue) {
    if (len > USB_STORAGE_BOUNCE_SIZE || cdb_len > 16)
        return USB_ERR;

    usb_bot_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.signature   = USB_BOT_CBW_SIGNATURE;
    cbw.tag         = ++ms->tag;
    cbw.data_length = len;
    cbw.flags       = in ? USB_BOT_CBW_IN : 0;
    cbw.lun         = ms->lun;
    cbw.cdb_length  = cdb_len;
    memcpy(cbw.cdb, cdb, cdb_len);
    memcpy(ms->cmd + USB_BOT_CBW_OFFSET, &cbw, sizeof(cbw));

    int r = usb_bulk(ms->dev, ms->ep_out, ms->cmd + USB_BOT_CBW_OFFSET, sizeof(cbw));
    if (r != (int)sizeof(cbw)) {
        serial_printf(LOG_WARN "usbstorage%d: CBW for scsi 0x%02x didnt go out (%d)\n",
                      ms->index, cdb[0], r);
        usb_bot_reset(ms);
        return USB_ERR;
    }

    // a stall in the data stage is the device saying "less than that", the CSW still follows
    if (len) {
        r = usb_bulk(ms->dev, in ? ms->ep_in : ms->ep_out, ms->bounce, len);
        if (r == USB_ERR_STALL) {
            serial_printf(LOG_INFO "usbstorage%d: scsi 0x%02x data stage stalled\n", ms->index, cdb[0]);
            usb_clear_halt(ms->dev, in ? ms->ep_in : ms->ep_out);
        } else if (r < 0) {
            serial_printf(LOG_WARN "usbstorage%d: scsi 0x%02x data stage failed (%d)\n",
                          ms->index, cdb[0], r);
            usb_bot_reset(ms);
            return USB_ERR;
        }
    }

    usb_bot_csw_t csw;
    if (usb_bot_read_csw(ms, &csw) < 0) {
        usb_bot_reset(ms);
        return USB_ERR;
    }

    if (csw.signature != USB_BOT_CSW_SIGNATURE || csw.tag != cbw.tag) {
        serial_printf(LOG_WARN "usbstorage%d: bad CSW, signature 0x%08x tag %u (wanted %u)\n",
                      ms->index, csw.signature, csw.tag, cbw.tag);
        usb_bot_reset(ms);
        return USB_ERR;
    }
    if (csw.status == USB_BOT_PHASE_ERROR) {
        serial_printf(LOG_WARN "usbstorage%d: scsi 0x%02x phase error\n", ms->index, cdb[0]);
        usb_bot_reset(ms);
        return USB_ERR;
    }

    if (residue)
        *residue = csw.residue;
    return csw.status == USB_BOT_PASSED ? USB_BOT_PASSED : USB_BOT_FAILED;
}
