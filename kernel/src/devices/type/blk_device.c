#include <devices/type/blk_device.h>
#include <drivers/ide/ide.h>
#include <string.h>

#define SECTOR_SIZE 512

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
        if (ide_read_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -1;

        size_t src_off = (total == 0) ? skip : 0;
        size_t avail   = SECTOR_SIZE - src_off;
        size_t chunk   = count - total;
        if (chunk > avail) chunk = avail;

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
        size_t avail   = SECTOR_SIZE - dst_off;
        size_t chunk   = count - total;
        if (chunk > avail) chunk = avail;

        if (dst_off != 0 || chunk < SECTOR_SIZE) {
            if (ide_read_sectors(blk->drive, abs_lba, 1, tmp) < 0)
                return total > 0 ? (long)total : -1;
        }

        memcpy(tmp + dst_off, (const uint8_t *)buf + total, chunk);

        if (ide_write_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -1;

        total += chunk;
        abs_lba++;
    }
    return (long)total;
}