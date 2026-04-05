#pragma once

#include <stdint.h>
#include <stddef.h>
#include <fs/vfs.h>
#include <drivers/ide/ide.h>

/*
 * sits between a raw IDE drive hardware and any filesystem driver
 * A whole-disk blk_device has lba_start=0, sector_count=drive->sector_count
 * A partition blk_device has lba_start and sector_count from the MBR entry
 */
typedef struct ide_drive ide_drive_t;

typedef struct blk_device {
    ide_drive_t *drive;
    uint32_t     lba_start;     // absolute LBA of first sector
    uint32_t     sector_count;  // size of this device in sectors */
    INode_t     *inode;
} blk_device_t;

// Read "count" bytes at byte "offset" from a block device into "buf".
long blk_read(blk_device_t *blk, void *buf, size_t count, size_t offset);

// Write "count" bytes at byte "offset" to a block device from "buf".
long blk_write(blk_device_t *blk, const void *buf, size_t count, size_t offset);