#include <drivers/ahci/ahci.h>
#include <drivers/pci/pci.h>
#include <drivers/ide/ide.h>
#include <devices/devices.h>
#include <devices/type/blk_device.h>
#include <fs/vfs.h>
#include <fs/partition.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <cpu/io.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>
#include <vendor/limine_bootloader/limine.h>
#include <cpu/control_registers.h>

#define ACHI_UPDATE_CMD_REG 0x80
#define AHCI_LBA_MODE       0x40

extern volatile struct limine_hhdm_request hhdm_request;

// offset helper
static inline uint64_t ahci_hhdm_offset(void) {
    return hhdm_request.response->offset;
}

#define PHYS(vptr)   ((uint64_t)(uintptr_t)(vptr) - ahci_hhdm_offset())
#define MMIO(paddr)  ((uint64_t)(paddr) + ahci_hhdm_offset())

#define AHCI_SECTOR_SIZE 512

static ahci_drive_t ahci_drives[AHCI_MAX_DRIVES];
static int          ahci_drive_count = 0;
static spinlock_t   ahci_lock = {0};

// mmio helpers
static inline uint32_t ahci_read(uint64_t abar, uint32_t offset) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(abar + offset);
    return *r;
}

static inline void ahci_write(uint64_t abar, uint32_t offset, uint32_t val) {
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(abar + offset);
    *r = val;
}

static inline uint32_t port_read(uint64_t abar, uint8_t port, uint32_t offset) {
    return ahci_read(abar, 0x100 + (uint32_t)port * 0x80 + offset);
}

static inline void port_write(uint64_t abar, uint8_t port,
                               uint32_t offset, uint32_t val) {
    ahci_write(abar, 0x100 + (uint32_t)port * 0x80 + offset, val);
}

// stop/start ports
static void ahci_port_stop(uint64_t abar, uint8_t port) {
    uint32_t cmd = port_read(abar, port, AHCI_PORT_CMD);
    cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);
    port_write(abar, port, AHCI_PORT_CMD, cmd);

    // Wait for CR and FR to clear
    for (int i = 0; i < 500; i++) {
        cmd = port_read(abar, port, AHCI_PORT_CMD);
        if (!(cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR)))
            return;
        
        for (volatile int d = 0; d < 1000; d++);    // arbitrary delay, sure hope processors dont get any faster
    }
    serial_printf(LOG_WARN "ahci: port %u stop timeout\n", port);
}

static void ahci_port_start(uint64_t abar, uint8_t port) {
    // wait for the cr to clear
    for (int i = 0; i < 500; i++) {
        if (!(port_read(abar, port, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR))
            break;
        for (volatile int d = 0; d < 1000; d++);
    }
    uint32_t cmd = port_read(abar, port, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_ST;
    port_write(abar, port, AHCI_PORT_CMD, cmd);
}

// wait for idle to clear (a command slot to become available)
static int ahci_port_wait_idle(uint64_t abar, uint8_t port) {
    for (int i = 0; i < 100000; i++) {
        uint32_t tfd = port_read(abar, port, AHCI_PORT_TFD);
        if (!(tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)))
            return 0;
        for (volatile int d = 0; d < 10; d++);
    }
    return -1;
}

static int ahci_port_wait_cmd(uint64_t abar, uint8_t port, uint32_t slot_mask) {
    for (int i = 0; i < 500000; i++) {
        uint32_t ci = port_read(abar, port, AHCI_PORT_CI);
        if (!(ci & slot_mask)) {
            /* Check for error */
            uint32_t is = port_read(abar, port, AHCI_PORT_IS);
            if (is & (1u << 30)) {   /* Task File Error Status */
                uint32_t tfd = port_read(abar, port, AHCI_PORT_TFD);
                serial_printf(LOG_ERROR "ahci: port %u cmd error IS=0x%08x TFD=0x%08x\n",
                              port, is, tfd);
                port_write(abar, port, AHCI_PORT_IS, is); /* clear */
                return -1;
            }
            return 0;
        }
        for (volatile int d = 0; d < 10; d++);
    }
    serial_printf(LOG_ERROR "ahci: port %u command timeout\n", port);
    return -2;
}

static bool ahci_port_init(ahci_drive_t *drive) {
    uint64_t abar = drive->abar;
    uint8_t  port = drive->port;

    ahci_port_stop(abar, port);

    drive->cmd_list  = kmalloc(sizeof(ahci_cmd_header_t) * 32);
    drive->fis_buf   = kmalloc(256);
    drive->cmd_table = kmalloc(sizeof(ahci_cmd_table_t));

    if (!drive->cmd_list || !drive->fis_buf || !drive->cmd_table) {
        serial_printf(LOG_ERROR "ahci: OOM during port %u init\n", port);
        return false;
    }

    memset(drive->cmd_list,  0, sizeof(ahci_cmd_header_t) * 32);
    memset(drive->fis_buf,   0, 256);
    memset(drive->cmd_table, 0, sizeof(ahci_cmd_table_t));

    // point the command header -> 0
    uint64_t ct_phys = PHYS(drive->cmd_table);
    drive->cmd_list[0].ctba  = (uint32_t)(ct_phys & 0xFFFFFFFF);
    drive->cmd_list[0].ctbau = (uint32_t)(ct_phys >> 32);

    // not too sure why these are done with the PADDR but i tried with the VADDR nad it didnt work
    uint64_t clb_phys = PHYS(drive->cmd_list);
    uint64_t fb_phys  = PHYS(drive->fis_buf);

    port_write(abar, port, AHCI_PORT_CLB,  (uint32_t)(clb_phys & 0xFFFFFFFF));
    port_write(abar, port, AHCI_PORT_CLBU, (uint32_t)(clb_phys >> 32));
    port_write(abar, port, AHCI_PORT_FB,   (uint32_t)(fb_phys  & 0xFFFFFFFF));
    port_write(abar, port, AHCI_PORT_FBU,  (uint32_t)(fb_phys  >> 32));

    // clear status registers
    port_write(abar, port, AHCI_PORT_SERR, 0xFFFFFFFF);
    port_write(abar, port, AHCI_PORT_IS,   0xFFFFFFFF);
    port_write(abar, port, AHCI_PORT_IE,   0);

    ahci_port_start(abar, port);
    return true;
}

// command sender (contains building)
static int ahci_issue_cmd(ahci_drive_t *drive, uint64_t lba, uint16_t count,
                           void *buf, bool write) {
    uint64_t abar = drive->abar;
    uint8_t  port = drive->port;

    // must be contiguous
    size_t   byte_count  = (size_t)count * AHCI_SECTOR_SIZE;
    uint32_t pages_needed = (uint32_t)((byte_count + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K);
    paddr_t  bounce_phys  = pmm_alloc_pages(pages_needed);
    if (!bounce_phys) {
        serial_printf(LOG_ERROR "ahci: OOM allocating bounce buffer (%u pages)\n", pages_needed);
        return -1;
    }
    void *bounce_virt = (void *)(uintptr_t)MMIO(bounce_phys);

    if (write)
        memcpy(bounce_virt, buf, byte_count);
    else
        memset(bounce_virt, 0, byte_count);

    if (ahci_port_wait_idle(abar, port) < 0) {
        serial_printf(LOG_ERROR "ahci: port %u busy before cmd\n", port);
        pmm_free_pages(bounce_phys, pages_needed);
        return -1;
    }

    port_write(abar, port, AHCI_PORT_IS,   0xFFFFFFFF);
    port_write(abar, port, AHCI_PORT_SERR, 0xFFFFFFFF);

    ahci_cmd_table_t *ct = drive->cmd_table;
    memset(ct, 0, sizeof(*ct));

    // h2d
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)ct->cfis;
    fis->fis_type  = FIS_TYPE_REG_H2D;
    fis->pmport_c  = ACHI_UPDATE_CMD_REG;
    fis->command   = write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    fis->device    = ATA_LBA_MODE;

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >>  8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    fis->countl = (uint8_t)(count);
    fis->counth = (uint8_t)(count >> 8);

    ct->prdt[0].dba  = (uint32_t)(bounce_phys & 0xFFFFFFFF);
    ct->prdt[0].dbau = (uint32_t)(bounce_phys >> 32);
    ct->prdt[0].dbc  = (uint32_t)(byte_count - 1);

    // build the command header
    ahci_cmd_header_t *hdr = &drive->cmd_list[0];
    hdr->flags = (uint16_t)((sizeof(fis_reg_h2d_t) / 4) // fis len in dwords
                            | (write ? (1u << 6) : 0));
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    port_write(abar, port, AHCI_PORT_CI, 1u << 0);

    int r = ahci_port_wait_cmd(abar, port, 1u << 0);

    if (r == 0 && !write)
        memcpy(buf, bounce_virt, byte_count);

    pmm_free_pages(bounce_phys, pages_needed);
    return r;
}

static bool ahci_identify(ahci_drive_t *drive) {
    uint64_t abar = drive->abar;
    uint8_t  port = drive->port;

    if (ahci_port_wait_idle(abar, port) < 0)
        return false;

    port_write(abar, port, AHCI_PORT_IS,   0xFFFFFFFF);
    port_write(abar, port, AHCI_PORT_SERR, 0xFFFFFFFF);

    paddr_t  id_phys = pmm_alloc_pages(1);
    if (!id_phys) return false;
    uint16_t *id = (uint16_t *)(uintptr_t)MMIO(id_phys);
    memset(id, 0, 512);

    ahci_cmd_table_t *ct = drive->cmd_table;
    memset(ct, 0, sizeof(*ct));

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)ct->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;
    fis->command  = ATA_CMD_IDENTIFY;
    fis->device   = 0;

    ct->prdt[0].dba  = (uint32_t)(id_phys & 0xFFFFFFFF);
    ct->prdt[0].dbau = (uint32_t)(id_phys >> 32);
    ct->prdt[0].dbc  = 511;

    ahci_cmd_header_t *hdr = &drive->cmd_list[0];
    hdr->flags = (uint16_t)(sizeof(fis_reg_h2d_t) / 4);
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    port_write(abar, port, AHCI_PORT_CI, 1u << 0);

    if (ahci_port_wait_cmd(abar, port, 1u << 0) < 0) {
        pmm_free_pages(id_phys, 1);
        return false;
    }

    drive->sector_count = ((uint64_t)id[103] << 48)
                        | ((uint64_t)id[102] << 32)
                        | ((uint64_t)id[101] << 16)
                        |  (uint64_t)id[100];

    if (drive->sector_count == 0)
        drive->sector_count = ((uint32_t)id[61] << 16) | id[60];

    // model str
    for (int i = 0; i < 20; i++) {
        drive->model[i * 2]     = (char)(id[27 + i] >> 8);
        drive->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    drive->model[40] = '\0';
    for (int i = 39; i >= 0 && drive->model[i] == ' '; i--)
        drive->model[i] = '\0';

    pmm_free_pages(id_phys, 1);
    return true;
}

int ahci_read_sectors(ahci_drive_t *drive, uint64_t lba,
                      uint16_t count, void *buf) {
    if (!drive || !drive->present || !buf || count == 0) return -1;
    spinlock_acquire(&ahci_lock);
    int r = ahci_issue_cmd(drive, lba, count, buf, false);
    spinlock_release(&ahci_lock);
    return r;
}

int ahci_write_sectors(ahci_drive_t *drive, uint64_t lba,
                       uint16_t count, const void *buf) {
    if (!drive || !drive->present || !buf || count == 0) return -1;
    spinlock_acquire(&ahci_lock);
    int r = ahci_issue_cmd(drive, lba, count, (void *)buf, true);
    spinlock_release(&ahci_lock);
    return r;
}

ahci_drive_t *ahci_get_drive(int index) {
    if (index < 0 || index >= ahci_drive_count) return NULL;
    return ahci_drives[index].present ? &ahci_drives[index] : NULL;
}

// Wrapper for partition_probe
static int ahci_read_sectors_wrapper(void *drive, uint64_t lba, uint16_t count, void *buf) {
    return ahci_read_sectors((ahci_drive_t *)drive, lba, count, buf);
}

typedef struct {
    ahci_drive_t *drive;
    uint64_t      lba_start;
    uint64_t      sector_count;
    INode_t      *inode;
} ahci_blk_t;

static long ahci_blk_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    ahci_blk_t *blk = inode->internal_data;
    if (!blk || count == 0) return 0;

    size_t max = (size_t)(blk->sector_count * AHCI_SECTOR_SIZE);
    if (offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    uint64_t abs_lba = blk->lba_start + offset / AHCI_SECTOR_SIZE;
    size_t   skip    = offset % AHCI_SECTOR_SIZE;
    size_t   total   = 0;
    uint8_t  tmp[AHCI_SECTOR_SIZE];

    while (total < count) {
        if (ahci_read_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -1;

        size_t src_off = (total == 0) ? skip : 0;
        size_t avail   = AHCI_SECTOR_SIZE - src_off;
        size_t chunk   = count - total;
        if (chunk > avail) chunk = avail;

        memcpy((uint8_t *)buf + total, tmp + src_off, chunk);
        total += chunk;
        abs_lba++;
    }
    return (long)total;
}

static long ahci_blk_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    ahci_blk_t *blk = inode->internal_data;
    if (!blk || count == 0) return 0;

    size_t max = (size_t)(blk->sector_count * AHCI_SECTOR_SIZE);
    if (offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    uint64_t abs_lba = blk->lba_start + offset / AHCI_SECTOR_SIZE;
    size_t   skip    = offset % AHCI_SECTOR_SIZE;
    size_t   total   = 0;
    uint8_t  tmp[AHCI_SECTOR_SIZE];

    while (total < count) {
        size_t dst_off = (total == 0) ? skip : 0;
        size_t avail   = AHCI_SECTOR_SIZE - dst_off;
        size_t chunk   = count - total;
        if (chunk > avail) chunk = avail;

        if (dst_off != 0 || chunk < AHCI_SECTOR_SIZE) {
            if (ahci_read_sectors(blk->drive, abs_lba, 1, tmp) < 0)
                return total > 0 ? (long)total : -1;
        }
        memcpy(tmp + dst_off, (const uint8_t *)buf + total, chunk);
        if (ahci_write_sectors(blk->drive, abs_lba, 1, tmp) < 0)
            return total > 0 ? (long)total : -1;

        total += chunk;
        abs_lba++;
    }
    return (long)total;
}

static size_t ahci_blk_size(INode_t *inode) {
    ahci_blk_t *blk = inode->internal_data;
    return blk ? (size_t)(blk->sector_count * AHCI_SECTOR_SIZE) : 0;
}

static INodeOps_t ahci_blk_ops = {
    .read  = ahci_blk_read,
    .write = ahci_blk_write,
    .size  = ahci_blk_size,
};

static void ahci_register_drive(int drive_idx, ahci_drive_t *drive) {
    char dev_name[8];
    snprintf(dev_name, sizeof(dev_name), "sd%c", 'a' + drive_idx);

    ahci_blk_t *blk = kmalloc(sizeof(*blk));
    if (!blk) return;
    memset(blk, 0, sizeof(*blk));
    blk->drive        = drive;
    blk->lba_start    = 0;
    blk->sector_count = drive->sector_count;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(blk, sizeof(*blk)); return; }
    memset(inode, 0, sizeof(*inode));
    inode->type          = INODE_DEVICE;
    inode->ops           = &ahci_blk_ops;
    inode->internal_data = blk;
    inode->shared        = 1;
    blk->inode           = inode;

    if (device_register(inode, dev_name) < 0) {
        serial_printf(LOG_WARN "ahci: failed to register /dev/%s\n", dev_name);
        kfree(inode, sizeof(*inode));
        kfree(blk, sizeof(*blk));
        return;
    }
    serial_printf(LOG_OK "ahci: /dev/%s - %s (%llu sectors)\n",
                  dev_name, drive->model, drive->sector_count);
}

// Callback invoked by partition_probe for each partition found
static void ahci_register_part_cb(void *drive_obj, int drive_idx, int part_idx, uint64_t start, uint64_t count, uint8_t type) {
    ahci_drive_t *drive = (ahci_drive_t *)drive_obj;
    char part_name[16];
    snprintf(part_name, sizeof(part_name), "sd%c%d", 'a' + drive_idx, part_idx + 1);

    ahci_blk_t *pblk = kmalloc(sizeof(*pblk));
    if (!pblk) return;
    memset(pblk, 0, sizeof(*pblk));
    pblk->drive        = drive;
    pblk->lba_start    = start;
    pblk->sector_count = count;

    INode_t *pinode = kmalloc(sizeof(*pinode));
    if (!pinode) { kfree(pblk, sizeof(*pblk)); return; }
    memset(pinode, 0, sizeof(*pinode));
    pinode->type          = INODE_DEVICE;
    pinode->ops           = &ahci_blk_ops;
    pinode->internal_data = pblk;
    pinode->shared        = 1;
    pblk->inode           = pinode;

    if (device_register(pinode, part_name) < 0) {
        serial_printf(LOG_WARN "ahci: failed to register /dev/%s\n", part_name);
        kfree(pinode, sizeof(*pinode));
        kfree(pblk, sizeof(*pblk));
        return;
    }
    serial_printf(LOG_OK "ahci: registered /dev/%s (lba=%llu sectors=%llu type=0x%02x)\n",
                  part_name, start, count, type);
}

void ahci_init(void) {
    memset(ahci_drives, 0, sizeof(ahci_drives));

    // locate ahci on pci
    pci_device_t pci_dev;
    if (!pci_find_device(PCI_CLASS_STORAGE, PCI_SUBCLASS_SATA,
                         PCI_PROGIF_AHCI, &pci_dev)) {
        serial_printf(LOG_INFO "ahci: no AHCI controller found\n");
        return;
    }

    serial_printf(LOG_OK "ahci: controller found - PCI %02x:%02x.%x "
                  "vendor=0x%04x device=0x%04x\n",
                  pci_dev.bus, pci_dev.device, pci_dev.function,
                  pci_dev.vendor_id, pci_dev.device_id);

    // Enable bus mastering and memory space in PCI command register
    uint16_t cmd = pci_read16(pci_dev.bus, pci_dev.device, pci_dev.function,
                              PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
    pci_write16(pci_dev.bus, pci_dev.device, pci_dev.function,
                PCI_COMMAND, cmd);

    // BAR5 AHCI base addr
    uint64_t abar_phys = pci_dev.bar[5] & ~0xFFFu;
    if (abar_phys == 0) {
        serial_printf(LOG_ERROR "ahci: ABAR is zero - controller not mapped\n");
        return;
    }

    // map the mmio region
    paddr_t cr3 = read_cr3();
    uint64_t abar_page  = abar_phys & ~(uint64_t)(PAGE_SIZE_4K - 1);
    uint64_t abar_end   = (abar_phys + 0x2000 + PAGE_SIZE_4K - 1)
                          & ~(uint64_t)(PAGE_SIZE_4K - 1);

    for (uint64_t p = abar_page; p < abar_end; p += PAGE_SIZE_4K) {
        uint64_t virt = p + ahci_hhdm_offset();
        uint64_t *pte = paging_get_page(cr3, virt, 1);
        if (pte) {
            *pte = (p & PADDR_ENTRY_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_NX;
            asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
        } else {
            serial_printf(LOG_ERROR "ahci: failed to map MMIO page 0x%llx\n", p);
            return;
        }
    }

    uint64_t abar = MMIO(abar_phys);
    serial_printf(LOG_INFO "ahci: ABAR phys=0x%llx virt=0x%llx (mapped)\n",
                  abar_phys, abar);

    uint32_t ghc = ahci_read(abar, AHCI_GHC_GHC);
    if (!(ghc & AHCI_GHC_ENABLE)) {
        ahci_write(abar, AHCI_GHC_GHC, ghc | AHCI_GHC_ENABLE);
    }

    uint32_t pi = ahci_read(abar, AHCI_GHC_PI);
    serial_printf(LOG_INFO "ahci: ports implemented = 0x%08x\n", pi);

    int drive_idx = 0;

    for (int port = 0; port < AHCI_MAX_PORTS && drive_idx < AHCI_MAX_DRIVES; port++) {
        if (!(pi & (1u << port))) continue;

        uint32_t ssts = port_read(abar, (uint8_t)port, AHCI_PORT_SSTS);
        if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PRESENT) continue;

        uint32_t sig = port_read(abar, (uint8_t)port, AHCI_PORT_SIG);
        if (sig == AHCI_SIG_ATAPI) {
            serial_printf(LOG_INFO "ahci: port %d is ATAPI - skipping\n", port);
            continue;
        }
        if (sig != AHCI_SIG_ATA) {
            serial_printf(LOG_INFO "ahci: port %d unknown sig 0x%08x - skipping\n",
                          port, sig);
            continue;
        }

        ahci_drive_t *drive = &ahci_drives[drive_idx];
        drive->abar    = abar;
        drive->port    = (uint8_t)port;
        drive->present = false;

        if (!ahci_port_init(drive)) {
            serial_printf(LOG_WARN "ahci: port %d init failed\n", port);
            continue;
        }

        if (!ahci_identify(drive)) {
            serial_printf(LOG_WARN "ahci: port %d IDENTIFY failed\n", port);
            continue;
        }

        drive->present = true;
        ahci_drive_count++;


        ahci_register_drive(drive_idx, drive);
        partition_probe(drive, drive_idx, ahci_read_sectors_wrapper, ahci_register_part_cb);
        drive_idx++;
    }

    if (drive_idx == 0)
        serial_printf(LOG_WARN "ahci: no SATA drives found\n");
}