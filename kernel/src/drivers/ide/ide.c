#include <drivers/ide/ide.h>
#include <devices/devices.h>
#include <devices/type/blk_device.h>
#include <fs/vfs.h>
#include <fs/partition.h>
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

static inline void ide_io_wait(void) { wait_ns(400); }

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

    // bounded, a drive that never raises DRQ shouldnt be able to hang the boot
    timeout = 100000;
    for (;;) {
        status = inb(base + ATA_REG_STATUS);
        if (status & ATA_SR_ERR)  return false;
        if (status & ATA_SR_DRQ) break;
        if (--timeout <= 0)      return false;
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
    ide_io_wait();

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

// Wrapper for partition_probe to bridge uint8_t count to uint16_t count ABI cleanly
static int ide_read_sectors_wrapper(void *drive, uint64_t lba, uint16_t count, void *buf) {
    return ide_read_sectors((ide_drive_t *)drive, (uint32_t)lba, (uint8_t)count, buf);
}

// register the device within bleed, block device inode ops
static long blk_inode_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    return blk_read(inode->internal_data, buf, count, offset);
}

static long blk_inode_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    return blk_write(inode->internal_data, buf, count, offset);
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
    if (!inode) { kfree(blk); return; }
    memset(inode, 0, sizeof(*inode));
    inode->type          = INODE_DEVICE;
    inode->ops           = &blk_inode_ops;
    inode->internal_data = blk;
    inode->shared        = 1;

    blk->inode = inode;

    if (device_register(inode, dev_name) < 0) {
        serial_printf(LOG_WARN "ide: failed to register /dev/%s\n", dev_name);
        kfree(inode);
        kfree(blk);
        return;
    }
    serial_printf(LOG_OK "ide: registered /dev/%s (%s, %u sectors)\n",
                  dev_name, drive->model, drive->sector_count);
}

// Callback invoked by partition_probe for each partition found
static void ide_register_part_cb(void *drive_obj, int drive_idx, int part_idx, uint64_t start, uint64_t count, uint8_t type) {
    ide_drive_t *drive = (ide_drive_t *)drive_obj;
    char part_name[16];
    snprintf(part_name, sizeof(part_name), "hd%c%d", 'a' + drive_idx, part_idx + 1);

    blk_device_t *pblk = kmalloc(sizeof(blk_device_t));
    if (!pblk) return;
    memset(pblk, 0, sizeof(*pblk));
    pblk->drive        = drive;
    pblk->lba_start    = (uint32_t)start;
    pblk->sector_count = (uint32_t)count;

    INode_t *pinode = kmalloc(sizeof(INode_t));
    if (!pinode) { kfree(pblk); return; }
    memset(pinode, 0, sizeof(*pinode));
    pinode->type          = INODE_DEVICE;
    pinode->ops           = &blk_inode_ops;
    pinode->internal_data = pblk;
    pinode->shared        = 1;
    pblk->inode           = pinode;

    if (device_register(pinode, part_name) < 0) {
        serial_printf(LOG_WARN "ide: failed to register /dev/%s\n", part_name);
        kfree(pinode);
        kfree(pblk);
        return;
    }
    serial_printf(LOG_OK "ide: registered /dev/%s (lba=%u sectors=%u type=0x%x)\n",
                  part_name, (uint32_t)start, (uint32_t)count, type);
}

void ide_init(void) {
    memset(ide_drives, 0, sizeof(ide_drives));

    outb(ATA_PRIMARY_CTRL,   0x04); /* assert SRST */
    outb(ATA_SECONDARY_CTRL, 0x04);
    for (int i = 0; i < 4; i++) inb(ATA_PRIMARY_CTRL);
    outb(ATA_PRIMARY_CTRL,   0x02); /* release SRST, set nIEN to disable IRQs */
    outb(ATA_SECONDARY_CTRL, 0x02);
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

        ide_register_drive(i, &ide_drives[i]);
        partition_probe(&ide_drives[i], i, ide_read_sectors_wrapper, ide_register_part_cb);
    }
}