#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// io ports for *base*
#define ATA_PRIMARY_BASE      0x1F0
#define ATA_PRIMARY_CTRL      0x3F6
#define ATA_SECONDARY_BASE    0x170
#define ATA_SECONDARY_CTRL    0x376

// ata registers
#define ATA_REG_DATA          0x00
#define ATA_REG_ERROR         0x01
#define ATA_REG_FEATURES      0x01
#define ATA_REG_SECTOR_COUNT  0x02
#define ATA_REG_LBA_LO        0x03
#define ATA_REG_LBA_MID       0x04
#define ATA_REG_LBA_HI        0x05
#define ATA_REG_DRIVE_HEAD    0x06
#define ATA_REG_STATUS        0x07
#define ATA_REG_COMMAND       0x07

// status bits
#define ATA_SR_BSY            0x80
#define ATA_SR_DRDY           0x40
#define ATA_SR_DRQ            0x08
#define ATA_SR_ERR            0x01

// ata cmds
#define ATA_CMD_READ_PIO      0x20
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

// drive select
#define ATA_DRIVE_MASTER      0xA0
#define ATA_DRIVE_SLAVE       0xB0
#define ATA_LBA_MODE          0x40 

#define IDE_SECTOR_SIZE       512
#define IDE_MAX_DRIVES        4
#define IDE_MAX_PARTITIONS    4

// mbr
#define MBR_SIGNATURE         0xAA55
#define MBR_PARTITION_OFFSET  446
#define MBR_PARTITION_COUNT   4

typedef struct __attribute__((packed)) mbr_partition_entry {
    uint8_t  status;          /* 0x80 = bootable */
    uint8_t  chs_first[3];
    uint8_t  type;            /* partition type */
    uint8_t  chs_last[3];
    uint32_t lba_start;       /* LBA of first sector */
    uint32_t sector_count;
} mbr_partition_entry_t;

typedef struct __attribute__((packed)) mbr {
    uint8_t              bootstrap[446];
    mbr_partition_entry_t partitions[4];
    uint16_t             signature;
} mbr_t;

typedef struct ide_drive {
    bool     present;
    bool     is_slave;
    uint16_t base;            // I/O base port
    uint16_t ctrl;            // control port 
    uint32_t sector_count;    // total LBA28 sectors 
    char     model[41];       // model string from IDENTIFY

    /* Partition table maxes out at 4 */
    struct {
        bool     present;
        uint8_t  type;
        uint32_t lba_start;
        uint32_t sector_count;
    } partitions[IDE_MAX_PARTITIONS];
} ide_drive_t;


// detects and registers all IDE drives, reads their MBR. also handles partitions
void ide_init(void);

/**
 * Read "count" sectors starting at "lba" from drive into "buf".
 * Returns 0 on success, negative on error.
 */
int ide_read_sectors(ide_drive_t *drive, uint32_t lba, uint8_t count, void *buf);

/**
 * Write "count" sectors starting at "lba" from "buf" to drive.
 * Returns 0 on success, negative on error.
 */
int ide_write_sectors(ide_drive_t *drive, uint32_t lba, uint8_t count, const void *buf);

/**
 * Return drive descriptor by index (0=primary master … 3=secondary slave).
 * Returns NULL if not present.
 */
ide_drive_t *ide_get_drive(int index);