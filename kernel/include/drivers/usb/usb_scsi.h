#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <drivers/usb/usb_storage.h>

// SCSI block device commands
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_START_STOP_UNIT    0x1B
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_READ_16            0x88
#define SCSI_WRITE_16           0x8A
#define SCSI_SERVICE_ACTION_IN  0x9E
#define SCSI_SAI_READ_CAPACITY  0x10

#define SCSI_START_STOP_EJECT   0x02

#define SCSI_INQUIRY_LEN        36
#define SCSI_SENSE_LEN          18
#define SCSI_TYPE_DISK          0x00
#define SCSI_SENSE_NOT_READY    0x02
#define SCSI_SENSE_UNIT_ATTENTION 0x06  // "i just got plugged in", always the first thing a stick says

#define SCSI_READY_TRIES        20
#define SCSI_READY_DELAY_MS     100

// Fill in vendor/product/revision, this isnt populated if its not a mass storage device or soemthing we can use as a disk
bool usb_scsi_inquiry(usb_storage_t *ms);

// Keep asking until the medium is ready, sticks take a moment after power on
bool usb_scsi_wait_ready(usb_storage_t *ms);

// Fill in sector_count and sector_size
bool usb_scsi_read_capacity(usb_storage_t *ms);

// Move count sectors between the disk and ms->bounce, count * sector_size has to fit in it
int usb_scsi_rw(usb_storage_t *ms, uint64_t lba, uint32_t count, bool write);

// Stop the medium and eject it if it can
int usb_scsi_eject(usb_storage_t *ms);
