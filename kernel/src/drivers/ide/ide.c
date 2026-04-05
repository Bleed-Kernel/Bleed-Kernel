#include <drivers/ide/ide.h>
#include <devices/devices.h>
#include <devices/type/blk_device.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>
#include <cpu/io.h>
#include <ACPI/acpi_hpet.h>

static ide_drive_t ide_drives[IDE_MAX_DRIVES];
static spinlock_t  ide_lock = {0};

static inline void ide_io_wait() { wait_ns(400); }

static int ide_poll(uint16_t base) {
    ide_io_wait();

    for (int i = 0; i < 30000; i++) {
        uint8_t status = inb(base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) {
            uint8_t err = inb(base + ATA_REG_ERROR);
            serial_printf(LOG_ERROR "ide: poll error - status=0x%x err=0x%x\n", status, err);
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
    serial_printf(LOG_ERROR "ide: poll timeout - status=0x%x\n", inb(base + ATA_REG_STATUS));
    return -2;
}

static int ide_wait_ready(uint16_t base) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(base + ATA_REG_STATUS);
        if (s == 0xFF) return -1; // bus is likely empty
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY)) return 0;
    }
    return -1;
}

static bool ide_identify(uint16_t base, uint16_t ctrl, bool slave, ide_drive_t *out) {
    uint8_t drive_sel = slave ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;

    outb(base + ATA_REG_DRIVE_HEAD, drive_sel);
    ide_io_wait();

    // zero the registers before identify
    outb(base + ATA_REG_SECTOR_COUNT, 0);
    outb(base + ATA_REG_LBA_LO,  0);
    outb(base + ATA_REG_LBA_MID, 0);
    outb(base + ATA_REG_LBA_HI,  0);
    outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ide_io_wait();

    uint8_t status = inb(base + ATA_REG_STATUS);
    if (status == 0) return false; // no drive???

    // wait for us to not be busy anymore
    int timeout = 100000;
    while ((inb(base + ATA_REG_STATUS) & ATA_SR_BSY) && timeout--);
    if (timeout <= 0) return false;

    /* Non-ATA drives (ATAPI) set LBA_MID/HI to non-zero */
    if (inb(base + ATA_REG_LBA_MID) || inb(base + ATA_REG_LBA_HI))
        return false;

    for (;;) {
        status = inb(base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return false;
        if (status & ATA_SR_DRQ) break;
    }

    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = inw(base + ATA_REG_DATA);

    out->sector_count = ((uint32_t)identify[61] << 16) | identify[60];
    out->base     = base;
    out->ctrl     = ctrl;
    out->is_slave = slave;
    out->present  = true;

    // big endian
    for (int i = 0; i < 20; i++) {
        out->model[i * 2]     = (char)(identify[27 + i] >> 8);
        out->model[i * 2 + 1] = (char)(identify[27 + i] & 0xFF);
    }
    out->model[40] = '\0';
    for (int i = 39; i >= 0 && out->model[i] == ' '; i--)
        out->model[i] = '\0';

    return true;
}

// rw sectors

int ide_read_sectors(ide_drive_t *drive, uint32_t lba, uint8_t count, void *buf) {
    if (!drive || !drive->present || !buf || count == 0)
        return -1;

    spinlock_acquire(&ide_lock);

    uint8_t drive_sel = (drive->is_slave ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER)
                        | ATA_LBA_MODE
                        | ((lba >> 24) & 0x0F);

    // we have to wait for the drive to respond first
    outb(drive->base + ATA_REG_DRIVE_HEAD, drive_sel);
    ide_io_wait();

    if (ide_wait_ready(drive->base) < 0) {
        serial_printf(LOG_ERROR "ide: drive not ready for read lba=%u\n", lba);
        spinlock_release(&ide_lock);
        return -1;
    }

    outb(drive->base + ATA_REG_SECTOR_COUNT,  count);
    outb(drive->base + ATA_REG_LBA_LO,  (uint8_t)(lba));
    outb(drive->base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(drive->base + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
    outb(drive->base + ATA_REG_COMMAND,  ATA_CMD_READ_PIO);

    // dummy read
    inb(drive->base + ATA_REG_STATUS);

    uint16_t *ptr = (uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ide_poll(drive->base) < 0) {
            spinlock_release(&ide_lock);
            return -1;
        }
        for (int w = 0; w < 256; w++)
            ptr[s * 256 + w] = inw(drive->base + ATA_REG_DATA);
    }

    spinlock_release(&ide_lock);
    return 0;
}

int ide_write_sectors(ide_drive_t *drive, uint32_t lba, uint8_t count, const void *buf) {
    if (!drive || !drive->present || !buf || count == 0)
        return -1;

    spinlock_acquire(&ide_lock);

    uint8_t drive_sel = (drive->is_slave ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER)
                        | ATA_LBA_MODE
                        | ((lba >> 24) & 0x0F);

    outb(drive->base + ATA_REG_DRIVE_HEAD, drive_sel);
    ide_io_wait(drive->ctrl);

    if (ide_wait_ready(drive->base) < 0) {
        serial_printf(LOG_ERROR "ide: drive not ready for write lba=%u\n", lba);
        spinlock_release(&ide_lock);
        return -1;
    }

    outb(drive->base + ATA_REG_SECTOR_COUNT,  count);
    outb(drive->base + ATA_REG_LBA_LO,  (uint8_t)(lba));
    outb(drive->base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(drive->base + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
    outb(drive->base + ATA_REG_COMMAND,  ATA_CMD_WRITE_PIO);

    uint8_t st = inb(drive->base + ATA_REG_STATUS);
    (void)st;

    for (int i = 0; i < 10; i++) inb(drive->ctrl);

    const uint16_t *ptr = (const uint16_t *)buf;
    for (int s = 0; s < count; s++) {
        if (ide_poll(drive->base) < 0) {
            spinlock_release(&ide_lock);
            return -1;
        }
        for (int w = 0; w < 256; w++)
            outw(drive->base + ATA_REG_DATA, ptr[s * 256 + w]);
    }

    outb(drive->base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ide_poll(drive->base);

    spinlock_release(&ide_lock);
    return 0;
}

ide_drive_t *ide_get_drive(int index) {
    if (index < 0 || index >= IDE_MAX_DRIVES) return NULL;
    return ide_drives[index].present ? &ide_drives[index] : NULL;
}

// MBR Parse

static void ide_read_partitions(ide_drive_t *drive) {
    uint8_t sector[IDE_SECTOR_SIZE];
    if (ide_read_sectors(drive, 0, 1, sector) < 0) {
        serial_printf(LOG_WARN "ide: failed to read MBR\n");
        return;
    }

    mbr_t *mbr = (mbr_t *)sector;
    if (mbr->signature != MBR_SIGNATURE) {
        serial_printf(LOG_WARN "ide: MBR signature invalid (0x%04x)\n", mbr->signature);
        return;
    }

    for (int i = 0; i < MBR_PARTITION_COUNT; i++) {
        mbr_partition_entry_t *p = &mbr->partitions[i];
        if (p->type == 0 || p->sector_count == 0)
            continue;
        drive->partitions[i].present      = true;
        drive->partitions[i].type         = p->type;
        drive->partitions[i].lba_start    = p->lba_start;
        drive->partitions[i].sector_count = p->sector_count;
        serial_printf(LOG_OK "ide: partition %d type=0x%x lba=%u sectors=%u\n",
                      i + 1, p->type, p->lba_start, p->sector_count);
    }
}

// register the device within bleed, block device inode ops

static long blk_inode_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    blk_device_t *blk = inode->internal_data;
    if (!blk || count == 0) return 0;

    uint32_t abs_lba   = blk->lba_start + (uint32_t)(offset / IDE_SECTOR_SIZE);
    size_t   skip      = offset % IDE_SECTOR_SIZE;
    size_t   total     = 0;
    uint8_t  sector_buf[IDE_SECTOR_SIZE];

    /* Clamp to partition boundary */
    size_t max_bytes = (size_t)blk->sector_count * IDE_SECTOR_SIZE;
    if (offset >= max_bytes) return 0;
    if (count > max_bytes - offset) count = max_bytes - offset;

    while (total < count) {
        if (ide_read_sectors(blk->drive, abs_lba, 1, sector_buf) < 0)
            return total > 0 ? (long)total : -1;

        size_t copy_off  = (total == 0) ? skip : 0;
        size_t available = IDE_SECTOR_SIZE - copy_off;
        size_t to_copy   = count - total;
        if (to_copy > available) to_copy = available;

        memcpy((uint8_t *)buf + total, sector_buf + copy_off, to_copy);
        total += to_copy;
        abs_lba++;
    }
    return (long)total;
}

static long blk_inode_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    blk_device_t *blk = inode->internal_data;
    if (!blk || count == 0) return 0;

    uint32_t abs_lba = blk->lba_start + (uint32_t)(offset / IDE_SECTOR_SIZE);
    size_t   skip    = offset % IDE_SECTOR_SIZE;
    size_t   total   = 0;
    uint8_t  sector_buf[IDE_SECTOR_SIZE];

    size_t max_bytes = (size_t)blk->sector_count * IDE_SECTOR_SIZE;
    if (offset >= max_bytes) return 0;
    if (count > max_bytes - offset) count = max_bytes - offset;

    while (total < count) {
        size_t copy_off  = (total == 0) ? skip : 0;
        size_t available = IDE_SECTOR_SIZE - copy_off;
        size_t to_copy   = count - total;
        if (to_copy > available) to_copy = available;

        /* Read-modify-write if we're doing a partial sector */
        if (copy_off != 0 || to_copy < IDE_SECTOR_SIZE) {
            if (ide_read_sectors(blk->drive, abs_lba, 1, sector_buf) < 0)
                return total > 0 ? (long)total : -1;
        }

        memcpy(sector_buf + copy_off, (const uint8_t *)buf + total, to_copy);

        if (ide_write_sectors(blk->drive, abs_lba, 1, sector_buf) < 0)
            return total > 0 ? (long)total : -1;

        total += to_copy;
        abs_lba++;
    }
    return (long)total;
}

static size_t blk_inode_size(INode_t *inode) {
    blk_device_t *blk = inode->internal_data;
    if (!blk) return 0;
    return (size_t)blk->sector_count * IDE_SECTOR_SIZE;
}

static INodeOps_t blk_inode_ops = {
    .read  = blk_inode_read,
    .write = blk_inode_write,
    .size  = blk_inode_size,
};

static void ide_register_drive(int drive_idx, ide_drive_t *drive) {
    /* Drive letter: 0→a, 1→b, 2→c, 3→d */
    char dev_name[8];
    snprintf(dev_name, sizeof(dev_name), "hd%c", 'a' + drive_idx);

    blk_device_t *blk = kmalloc(sizeof(blk_device_t));
    if (!blk) return;
    memset(blk, 0, sizeof(*blk));
    blk->drive        = drive;
    blk->lba_start    = 0;
    blk->sector_count = drive->sector_count;

    INode_t *inode = kmalloc(sizeof(INode_t));
    if (!inode) { kfree(blk, sizeof(*blk)); return; }
    memset(inode, 0, sizeof(*inode));
    inode->type          = INODE_DEVICE;
    inode->ops           = &blk_inode_ops;
    inode->internal_data = blk;
    inode->shared        = 1;

    /* Store inode pointer inside blk so partitions can reference it */
    blk->inode = inode;

    if (device_register(inode, dev_name) < 0) {
        serial_printf(LOG_WARN "ide: failed to register /dev/%s\n", dev_name);
        kfree(inode, sizeof(*inode));
        kfree(blk, sizeof(*blk));
        return;
    }
    serial_printf(LOG_OK "ide: registered /dev/%s (%s, %u sectors)\n",
                  dev_name, drive->model, drive->sector_count);

    /* Register partitions */
    for (int p = 0; p < IDE_MAX_PARTITIONS; p++) {
        if (!drive->partitions[p].present) continue;

        char part_name[12];
        snprintf(part_name, sizeof(part_name), "hd%c%d", 'a' + drive_idx, p + 1);

        blk_device_t *pblk = kmalloc(sizeof(blk_device_t));
        if (!pblk) continue;
        memset(pblk, 0, sizeof(*pblk));
        pblk->drive        = drive;
        pblk->lba_start    = drive->partitions[p].lba_start;
        pblk->sector_count = drive->partitions[p].sector_count;

        INode_t *pinode = kmalloc(sizeof(INode_t));
        if (!pinode) { kfree(pblk, sizeof(*pblk)); continue; }
        memset(pinode, 0, sizeof(*pinode));
        pinode->type          = INODE_DEVICE;
        pinode->ops           = &blk_inode_ops;
        pinode->internal_data = pblk;
        pinode->shared        = 1;
        pblk->inode           = pinode;

        if (device_register(pinode, part_name) < 0) {
            serial_printf(LOG_WARN "ide: failed to register /dev/%s\n", part_name);
            kfree(pinode, sizeof(*pinode));
            kfree(pblk, sizeof(*pblk));
            continue;
        }
        serial_printf(LOG_OK "ide: registered /dev/%s (lba=%u sectors=%u type=0x%x)\n",
                      part_name,
                      drive->partitions[p].lba_start,
                      drive->partitions[p].sector_count,
                      drive->partitions[p].type);
    }
}

/* ── ide_init ─────────────────────────────────────────────────────────── */

/*
 * Drive index layout:
 *   0 = primary master   (base=0x1F0, slave=false)
 *   1 = primary slave    (base=0x1F0, slave=true)
 *   2 = secondary master (base=0x170, slave=false)
 *   3 = secondary slave  (base=0x170, slave=true)
 */
void ide_init(void) {
    memset(ide_drives, 0, sizeof(ide_drives));

    /*
     * Software reset both buses before probing. Writing bit 2 (SRST) to the
     * device control register resets the bus; clearing it releases it.
     * This puts the controller in a known state regardless of what BIOS left.
     */
    outb(ATA_PRIMARY_CTRL,   0x04); /* assert SRST */
    outb(ATA_SECONDARY_CTRL, 0x04);
    /* Hold for ~5us - a few port reads is enough at PIO speeds */
    for (int i = 0; i < 4; i++) inb(ATA_PRIMARY_CTRL);
    outb(ATA_PRIMARY_CTRL,   0x02); /* release SRST, set nIEN to disable IRQs */
    outb(ATA_SECONDARY_CTRL, 0x02);
    /* Give drives time to complete reset - ~2ms */
    for (int i = 0; i < 4000; i++) inb(ATA_PRIMARY_CTRL);

    struct { uint16_t base; uint16_t ctrl; bool slave; } probes[IDE_MAX_DRIVES] = {
        { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   false },
        { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   true  },
        { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, false },
        { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, true  },
    };

    for (int i = 0; i < IDE_MAX_DRIVES; i++) {
        if (!ide_identify(probes[i].base, probes[i].ctrl, probes[i].slave, &ide_drives[i])) {
            serial_printf(LOG_INFO "ide: slot %d - not present\n", i);
            continue;
        }

        ide_read_partitions(&ide_drives[i]);
        ide_register_drive(i, &ide_drives[i]);
    }
}