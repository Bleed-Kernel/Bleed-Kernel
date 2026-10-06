#include <devices/type/blk_device.h>
#include <drivers/ide/ide.h>
#include <string.h>
#include <user/errno.h>

#define SECTOR_SIZE 512
#define MAX_SECTORS_PER_CMD 255   // the count is one byte in an LBA28 command

long blk_read(blk_device_t *blk, void *buf, size_t count, size_t offset) {
    if (!blk || !buf || count == 0) return 0;

    size_t max = (size_t)blk->sector_count * SECTOR_SIZE;
    if (offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    uint32_t abs_lba = blk->lba_start + (uint32_t)(offset / SECTOR_SIZE);
    size_t   skip    = offset % SECTOR_SIZE;
    size_t   total   = 0;
    uint8_t  tmp[SECTOR_SIZE];

    while (total < count) {
        size_t src_off = (total == 0) ? skip : 0;
        size_t left    = count - total;

        // whole sectors go straight into the callers buffer, as many as one command takes
        if (src_off == 0 && left >= SECTOR_SIZE) {
            size_t sectors = left / SECTOR_SIZE;
            if (sectors > MAX_SECTORS_PER_CMD) sectors = MAX_SECTORS_PER_CMD;

            if (ide_read_sectors(blk->drive, abs_lba, (uint8_t)sectors, (uint8_t *)buf + total) < 0)
                return total > 0 ? (long)total : -EIO;

            total   += sectors * SECTOR_SIZE;
            abs_lba += (uint32_t)sectors;
            continue;
        }

        // partial sector at either end has to bounce through tmp
        if (ide_read_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -EIO;

        size_t avail = SECTOR_SIZE - src_off;
        size_t chunk = left < avail ? left : avail;

        memcpy((uint8_t *)buf + total, tmp + src_off, chunk);
        total += chunk;
        abs_lba++;
    }
    return (long)total;
}

long blk_write(blk_device_t *blk, const void *buf, size_t count, size_t offset) {
    if (!blk || !buf || count == 0) return 0;

    size_t max = (size_t)blk->sector_count * SECTOR_SIZE;
    if (offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    uint32_t abs_lba = blk->lba_start + (uint32_t)(offset / SECTOR_SIZE);
    size_t   skip    = offset % SECTOR_SIZE;
    size_t   total   = 0;
    uint8_t  tmp[SECTOR_SIZE];

    while (total < count) {
        size_t dst_off = (total == 0) ? skip : 0;
        size_t left    = count - total;

        if (dst_off == 0 && left >= SECTOR_SIZE) {
            size_t sectors = left / SECTOR_SIZE;
            if (sectors > MAX_SECTORS_PER_CMD) sectors = MAX_SECTORS_PER_CMD;

            if (ide_write_sectors(blk->drive, abs_lba, (uint8_t)sectors, (const uint8_t *)buf + total) < 0)
                return total > 0 ? (long)total : -EIO;

            total   += sectors * SECTOR_SIZE;
            abs_lba += (uint32_t)sectors;
            continue;
        }

        // Read-modify-write, we are only replacing part of this sector
        size_t avail = SECTOR_SIZE - dst_off;
        size_t chunk = left < avail ? left : avail;

        if (ide_read_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -EIO;

        memcpy(tmp + dst_off, (const uint8_t *)buf + total, chunk);

        if (ide_write_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -EIO;

        total += chunk;
        abs_lba++;
    }
    return (long)total;
}
