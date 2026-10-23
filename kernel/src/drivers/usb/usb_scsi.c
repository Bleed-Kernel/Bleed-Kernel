#include <drivers/usb/usb_scsi.h>
#include <drivers/usb/usb_bot.h>
#include <drivers/usb/usb_hcd.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

// scsi is big endian all the way through
static uint32_t scsi_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t scsi_be64(const uint8_t *p) {
    return (uint64_t)scsi_be32(p) << 32 | scsi_be32(p + 4);
}

static void scsi_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void scsi_put_be64(uint8_t *p, uint64_t v) {
    scsi_put_be32(p, (uint32_t)(v >> 32));
    scsi_put_be32(p + 4, (uint32_t)v);
}

// inquiry strings are space padded, not terminated
static void scsi_copy_string(char *dst, const uint8_t *src, size_t len) {
    memcpy(dst, src, len);
    dst[len] = '\0';
    for (size_t i = len; i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0'); i--)
        dst[i - 1] = '\0';
}

// after a FAILED status the reason sits on the device until someone asks for it
static void usb_scsi_sense(usb_storage_t *ms, uint8_t failed_op, uint8_t *key_out) {
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, SCSI_SENSE_LEN, 0 };

    if (usb_bot_command(ms, cdb, sizeof(cdb), true, SCSI_SENSE_LEN, NULL) != USB_BOT_PASSED) {
        serial_printf(LOG_WARN "usbstorage%d: scsi 0x%02x failed and REQUEST SENSE did too\n",
                      ms->index, failed_op);
        if (key_out)
            *key_out = 0xFF;
        return;
    }

    uint8_t key  = ms->bounce[2] & 0xF;
    uint8_t asc  = ms->bounce[12];
    uint8_t ascq = ms->bounce[13];
    serial_printf(LOG_INFO "usbstorage%d: scsi 0x%02x failed - sense key 0x%x asc 0x%02x ascq 0x%02x\n",
                  ms->index, failed_op, key, asc, ascq);
    if (key_out)
        *key_out = key;
}

// a command plus the sense lookup if it fails, returns 0 when it passed
static int usb_scsi_command(usb_storage_t *ms, const uint8_t *cdb, uint8_t cdb_len,
                            bool in, uint32_t len, uint8_t *sense_key) {
    uint32_t residue = 0;
    int r = usb_bot_command(ms, cdb, cdb_len, in, len, &residue);

    if (r == USB_BOT_FAILED) {
        usb_scsi_sense(ms, cdb[0], sense_key);
        return USB_ERR;
    }
    if (r < 0)
        return r;

    if (residue)
        serial_printf(LOG_INFO "usbstorage%d: scsi 0x%02x left %u of %u bytes unmoved\n",
                      ms->index, cdb[0], residue, len);
    return 0;
}

bool usb_scsi_inquiry(usb_storage_t *ms) {
    uint8_t cdb[6] = { SCSI_INQUIRY, 0, 0, 0, SCSI_INQUIRY_LEN, 0 };
    if (usb_scsi_command(ms, cdb, sizeof(cdb), true, SCSI_INQUIRY_LEN, NULL) < 0) {
        serial_printf(LOG_WARN "usbstorage%d: INQUIRY failed\n", ms->index);
        return false;
    }

    uint8_t type = ms->bounce[0] & 0x1F;
    scsi_copy_string(ms->vendor,   &ms->bounce[8],  8);
    scsi_copy_string(ms->product,  &ms->bounce[16], 16);
    scsi_copy_string(ms->revision, &ms->bounce[32], 4);

    serial_printf(LOG_INFO "usbstorage%d: INQUIRY - type 0x%02x, removable %u, \"%s\" \"%s\" \"%s\"\n",
                  ms->index, type, !!(ms->bounce[1] & 0x80), ms->vendor, ms->product, ms->revision);

    if (type != SCSI_TYPE_DISK) {
        serial_printf(LOG_WARN "usbstorage%d: device type 0x%02x isnt a disk, skipping it\n",
                      ms->index, type);
        return false;
    }
    return true;
}

bool usb_scsi_wait_ready(usb_storage_t *ms) {
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

    for (int attempt = 1; attempt <= SCSI_READY_TRIES; attempt++) {
        uint8_t key = 0;
        if (usb_scsi_command(ms, cdb, sizeof(cdb), false, 0, &key) == 0) {
            serial_printf(LOG_INFO "usbstorage%d: ready after %d tries\n", ms->index, attempt);
            return true;
        }

        // unit attention is just the device saying it was reset, ask again straight away
        if (key != SCSI_SENSE_UNIT_ATTENTION)
            usb_delay_ms(SCSI_READY_DELAY_MS);
    }

    serial_printf(LOG_WARN "usbstorage%d: never became ready, is there media in it?\n", ms->index);
    return false;
}

bool usb_scsi_read_capacity(usb_storage_t *ms) {
    uint8_t cdb10[10] = { SCSI_READ_CAPACITY_10 };
    if (usb_scsi_command(ms, cdb10, sizeof(cdb10), true, 8, NULL) < 0) {
        serial_printf(LOG_WARN "usbstorage%d: READ CAPACITY(10) failed\n", ms->index);
        return false;
    }

    uint64_t last_lba = scsi_be32(&ms->bounce[0]);
    ms->sector_size   = scsi_be32(&ms->bounce[4]);

    // all ones means the disk is too big for a 32bit lba and wants the 16 byte version
    if (last_lba == 0xFFFFFFFF) {
        uint8_t cdb16[16] = { SCSI_SERVICE_ACTION_IN, SCSI_SAI_READ_CAPACITY };
        scsi_put_be32(&cdb16[10], 32);
        if (usb_scsi_command(ms, cdb16, sizeof(cdb16), true, 32, NULL) < 0) {
            serial_printf(LOG_WARN "usbstorage%d: READ CAPACITY(16) failed\n", ms->index);
            return false;
        }
        last_lba        = scsi_be64(&ms->bounce[0]);
        ms->sector_size = scsi_be32(&ms->bounce[8]);
        ms->cdb16       = true;
    }

    ms->sector_count = last_lba + 1;

    if (ms->sector_size == 0 || ms->sector_size > USB_STORAGE_BOUNCE_SIZE ||
        (ms->sector_size & (ms->sector_size - 1))) {
        serial_printf(LOG_WARN "usbstorage%d: sector size %u makes no sense\n", ms->index, ms->sector_size);
        return false;
    }

    serial_printf(LOG_INFO "usbstorage%d: %llu sectors of %u bytes (%llu MiB)%s\n", ms->index,
                  ms->sector_count, ms->sector_size,
                  (ms->sector_count * ms->sector_size) >> 20, ms->cdb16 ? ", using 16 byte cdbs" : "");
    return true;
}

int usb_scsi_rw(usb_storage_t *ms, uint64_t lba, uint32_t count, bool write) {
    if (count == 0 || (uint64_t)count * ms->sector_size > USB_STORAGE_BOUNCE_SIZE)
        return USB_ERR;
    if (lba + count > ms->sector_count)
        return USB_ERR;

    uint8_t cdb[16];
    uint8_t cdb_len;
    memset(cdb, 0, sizeof(cdb));

    if (ms->cdb16) {
        cdb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
        scsi_put_be64(&cdb[2], lba);
        scsi_put_be32(&cdb[10], count);
        cdb_len = 16;
    } else {
        cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        scsi_put_be32(&cdb[2], (uint32_t)lba);
        cdb[7] = (uint8_t)(count >> 8);
        cdb[8] = (uint8_t)count;
        cdb_len = 10;
    }

    int r = usb_scsi_command(ms, cdb, cdb_len, !write, count * ms->sector_size, NULL);
    if (r < 0)
        serial_printf(LOG_WARN "usbstorage%d: %s of %u sectors at lba %llu failed\n",
                      ms->index, write ? "write" : "read", count, lba);
    return r;
}

int usb_scsi_eject(usb_storage_t *ms) {
    uint8_t cdb[6] = { SCSI_START_STOP_UNIT, 0, 0, 0, SCSI_START_STOP_EJECT, 0 };
    int r = usb_scsi_command(ms, cdb, sizeof(cdb), false, 0, NULL);
    if (r < 0)
        serial_printf(LOG_INFO "usbstorage%d: START STOP UNIT not taken (%d)\n", ms->index, r);
    else
        serial_printf(LOG_INFO "usbstorage%d: stopped and ejected\n", ms->index);
    return r;
}
