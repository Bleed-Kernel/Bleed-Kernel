#include <drivers/nvme/nvme.h>
#include <drivers/pci/pci.h>
#include <devices/devices.h>
#include <fs/vfs.h>
#include <fs/partition.h>
#include <mm/kalloc.h>
#include <mm/pmm.h>
#include <mm/paging.h>
#include <mm/spinlock.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>
#include <vendor/limine_bootloader/limine.h>
#include <cpu/control_registers.h>

extern volatile struct limine_hhdm_request hhdm_request;

static inline uint64_t nvme_hhdm_offset(void) {
    return hhdm_request.response->offset;
}

#define PHYS(vptr)  ((uint64_t)(uintptr_t)(vptr) - nvme_hhdm_offset())
#define MMIO(paddr) ((uint64_t)(paddr) + nvme_hhdm_offset())

static inline uint32_t nvme_read32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(uintptr_t)(base + off);
}
static inline void nvme_write32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(base + off) = val;
}
static inline uint64_t nvme_read64(uint64_t base, uint32_t off) {
    uint64_t lo = nvme_read32(base, off);
    uint64_t hi = nvme_read32(base, off + 4);
    return lo | (hi << 32);
}
static inline void nvme_write64(uint64_t base, uint32_t off, uint64_t val) {
    nvme_write32(base, off,     (uint32_t)(val & 0xFFFFFFFF));
    nvme_write32(base, off + 4, (uint32_t)(val >> 32));
}

// Doorbell register offset for a given queue/direction
static inline uint32_t sq_tail_db_off(nvme_drive_t *d, uint16_t qid) {
    return 0x1000 + (uint32_t)(2 * qid) * d->doorbell_stride;
}
static inline uint32_t cq_head_db_off(nvme_drive_t *d, uint16_t qid) {
    return 0x1000 + (uint32_t)(2 * qid + 1) * d->doorbell_stride;
}

static int nvme_wait_ready(nvme_drive_t *d, bool want_ready) {
    for (int i = 0; i < 2000000; i++) {
        uint32_t csts = nvme_read32(d->mmio_base, NVME_REG_CSTS);
        if (csts & NVME_CSTS_CFS) {
            serial_printf(LOG_ERROR "nvme: controller fatal status\n");
            return -1;
        }
        bool rdy = !!(csts & NVME_CSTS_RDY);
        if (rdy == want_ready) return 0;
        for (volatile int d2 = 0; d2 < 100; d2++);
    }
    serial_printf(LOG_ERROR "nvme: timeout waiting for CSTS.RDY=%d\n", (int)want_ready);
    return -1;
}

static uint16_t nvme_next_cid(nvme_drive_t *d) {
    return d->next_cid++;
}

static void nvme_admin_submit(nvme_drive_t *d, nvme_sqe_t *cmd) {
    uint16_t slot = d->asq_tail % NVME_ADMIN_QUEUE_DEPTH;
    memcpy(&d->asq[slot], cmd, sizeof(*cmd));
    d->asq_tail = (d->asq_tail + 1) % NVME_ADMIN_QUEUE_DEPTH;
    nvme_write32(d->mmio_base, sq_tail_db_off(d, 0), d->asq_tail);
}

// Poll admin CQ for a completion matching cid, returns status word
static int nvme_admin_poll(nvme_drive_t *d, uint16_t cid) {
    for (int i = 0; i < 2000000; i++) {
        volatile nvme_cqe_t *cqe = &d->acq[d->acq_head];
        asm volatile("" ::: "memory");  // prevent caching CQE reads
        uint8_t phase = cqe->status & 1;
        if (phase != d->acq_phase) {
            // not our entry yet
            for (volatile int w = 0; w < 10; w++);
            continue;
        }
        // entry valid
        uint16_t sc = (cqe->status >> 1) & 0x7FFF;
        d->acq_head = (d->acq_head + 1) % NVME_ADMIN_QUEUE_DEPTH;
        if (d->acq_head == 0) d->acq_phase ^= 1;
        // advance CQ head doorbell
        nvme_write32(d->mmio_base, cq_head_db_off(d, 0), d->acq_head);
        if (sc != 0) {
            serial_printf(LOG_ERROR "nvme: admin cmd cid=%u failed sc=0x%03x\n", cid, sc);
            return -1;
        }
        return 0;
    }
    serial_printf(LOG_ERROR "nvme: admin poll timeout\n");
    return -1;
}

// submit + poll one admin command helper
static int nvme_admin_cmd(nvme_drive_t *d, nvme_sqe_t *cmd) {
    uint16_t cid = nvme_next_cid(d);
    cmd->cid = cid;
    nvme_admin_submit(d, cmd);
    return nvme_admin_poll(d, cid);
}

static void nvme_io_submit(nvme_drive_t *d, nvme_sqe_t *cmd) {
    uint16_t slot = d->iosq_tail % NVME_IO_QUEUE_DEPTH;
    memcpy(&d->iosq[slot], cmd, sizeof(*cmd));
    d->iosq_tail = (d->iosq_tail + 1) % NVME_IO_QUEUE_DEPTH;
    nvme_write32(d->mmio_base, sq_tail_db_off(d, 1), d->iosq_tail);
}

static int nvme_io_poll(nvme_drive_t *d, uint16_t cid) {
    for (int i = 0; i < 2000000; i++) {
        volatile nvme_cqe_t *cqe = &d->iocq[d->iocq_head];
        asm volatile("" ::: "memory");  // prevent caching CQE reads
        uint8_t phase = cqe->status & 1;
        if (phase != d->iocq_phase) {
            for (volatile int w = 0; w < 10; w++);
            continue;
        }
        uint16_t sc = (cqe->status >> 1) & 0x7FFF;
        d->iocq_head = (d->iocq_head + 1) % NVME_IO_QUEUE_DEPTH;
        if (d->iocq_head == 0) d->iocq_phase ^= 1;
        nvme_write32(d->mmio_base, cq_head_db_off(d, 1), d->iocq_head);
        if (sc != 0) {
            serial_printf(LOG_ERROR "nvme: io cmd cid=%u failed sc=0x%03x\n", cid, sc);
            return -1;
        }
        return 0;
    }
    serial_printf(LOG_ERROR "nvme: io poll timeout\n");
    return -1;
}

static bool nvme_controller_init(nvme_drive_t *d) {
    // Disable controller first
    uint32_t cc = nvme_read32(d->mmio_base, NVME_REG_CC);
    if (cc & NVME_CC_EN) {
        nvme_write32(d->mmio_base, NVME_REG_CC, cc & ~NVME_CC_EN);
        if (nvme_wait_ready(d, false) < 0) return false;
    }

    // extract doorbell stride and minimum host page size
    uint64_t cap = nvme_read64(d->mmio_base, NVME_REG_CAP);
    uint8_t dstrd  = (uint8_t)((cap >> 32) & 0xF);
    uint8_t mpsmin = (uint8_t)((cap >> 48) & 0xF);
    d->doorbell_stride = 4u << dstrd;
    uint8_t mps = mpsmin;
    serial_printf(LOG_INFO "nvme: dstrd=%u mpsmin=%u mps=%u\n", dstrd, mpsmin, mps);

    // Admin queues MUST be 4096-byte aligned
    d->asq = (nvme_sqe_t *)(uintptr_t)MMIO(pmm_alloc_pages(1));
    d->acq = (nvme_cqe_t *)(uintptr_t)MMIO(pmm_alloc_pages(1));
    if (!d->asq || !d->acq) {
        serial_printf(LOG_ERROR "nvme: OOM allocating admin queues\n");
        return false;
    }
    memset(d->asq, 0, PAGE_SIZE_4K);
    memset(d->acq, 0, PAGE_SIZE_4K);

    d->asq_tail  = 0;
    d->acq_head  = 0;
    d->acq_phase = 1;
    d->next_cid  = 1;

    // Tell the controller about the admin queues
    uint32_t aqa = ((NVME_ADMIN_QUEUE_DEPTH - 1) << 16) | (NVME_ADMIN_QUEUE_DEPTH - 1);
    nvme_write32(d->mmio_base, NVME_REG_AQA, aqa);
    nvme_write64(d->mmio_base, NVME_REG_ASQ, PHYS(d->asq));
    nvme_write64(d->mmio_base, NVME_REG_ACQ, PHYS(d->acq));

    // Enable the controller
    cc = NVME_CC_EN | NVME_CC_CSS_NVM | ((uint32_t)mps << 7) |
         NVME_CC_AMS_RR | NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(d->mmio_base, NVME_REG_CC, cc);

    if (nvme_wait_ready(d, true) < 0) return false;

    for (volatile int w = 0; w < 100000; w++);

    serial_printf(LOG_OK "nvme: controller ready (CAP=0x%llx)\n", cap);
    return true;
}

static bool nvme_set_num_queues(nvme_drive_t *d, uint16_t num_queues) {
    nvme_sqe_t cmd = {0};
    cmd.opcode = 0x09;  // Set Features
    cmd.nsid   = 0;
    cmd.cdw10  = 0x07;

    uint16_t n = num_queues - 1;  // 0-based
    cmd.cdw11  = ((uint32_t)n << 16) | n;

    if (nvme_admin_cmd(d, &cmd) < 0) {
        serial_printf(LOG_ERROR "nvme: Set Features (num queues) failed\n");
        return false;
    }
    serial_printf(LOG_OK "nvme: negotiated %u IO queue pair(s)\n", num_queues);
    return true;
}

static bool nvme_create_io_queues(nvme_drive_t *d) {
    // IO queues also require page alignment
    d->iosq = (nvme_sqe_t *)(uintptr_t)MMIO(pmm_alloc_pages(1));
    d->iocq = (nvme_cqe_t *)(uintptr_t)MMIO(pmm_alloc_pages(1));
    if (!d->iosq || !d->iocq) {
        serial_printf(LOG_ERROR "nvme: OOM allocating IO queues\n");
        return false;
    }
    memset(d->iosq, 0, PAGE_SIZE_4K);
    memset(d->iocq, 0, PAGE_SIZE_4K);

    d->iosq_tail  = 0;
    d->iocq_head  = 0;
    d->iocq_phase = 1;

    // Create IO Completion Queue
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_ADM_CREATE_CQ;
    cmd.prp1   = PHYS(d->iocq);
    cmd.cdw10  = ((NVME_IO_QUEUE_DEPTH - 1) << 16) | 1; // size | qid
    cmd.cdw11  = (0 << 16) | 1;    //physically contiguous
    if (nvme_admin_cmd(d, &cmd) < 0) {
        serial_printf(LOG_ERROR "nvme: Create CQ failed\n");
        return false;
    }

    // Create IO Submission Queue
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADM_CREATE_SQ;
    cmd.prp1   = PHYS(d->iosq);
    cmd.cdw10  = ((NVME_IO_QUEUE_DEPTH - 1) << 16) | 1;
    cmd.cdw11  = (1 << 16) | (1 << 1) | 1;
    if (nvme_admin_cmd(d, &cmd) < 0) {
        serial_printf(LOG_ERROR "nvme: Create SQ failed\n");
        return false;
    }

    return true;
}

static bool nvme_identify_ns(nvme_drive_t *d, uint32_t nsid) {
    // Identify data buffer must be aligned to page
    nvme_identify_ns_t *ns = (nvme_identify_ns_t *)(uintptr_t)MMIO(pmm_alloc_pages(1));
    if (!ns) return false;
    memset(ns, 0, PAGE_SIZE_4K);

    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_ADM_IDENTIFY;
    cmd.nsid   = nsid;
    cmd.prp1   = PHYS(ns);
    cmd.cdw10  = 0;     // CNS=0 → Identify Namespace

    if (nvme_admin_cmd(d, &cmd) < 0) {
        pmm_free_pages(PHYS(ns), 1);
        return false;
    }

    d->sector_count = ns->nsze;
    uint8_t lbaf_idx = ns->flbas & 0xF;
    uint8_t lbads    = ns->lbaf[lbaf_idx].lbads;
    d->sector_size   = lbads ? (1u << lbads) : 512;
    d->nsid          = nsid;

    pmm_free_pages(PHYS(ns), 1);
    return true;
}

// Identify Controller to pull the model string
static bool nvme_identify_ctrl(nvme_drive_t *d) {
    uint8_t *data = (uint8_t *)(uintptr_t)MMIO(pmm_alloc_pages(1));
    if (!data) return false;
    memset(data, 0, PAGE_SIZE_4K);

    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_ADM_IDENTIFY;
    cmd.nsid   = 0;
    cmd.prp1   = PHYS(data);
    cmd.cdw10  = 1;

    if (nvme_admin_cmd(d, &cmd) < 0) {
        pmm_free_pages(PHYS(data), 1);
        return false;
    }

    // bytes 24-63 = model number (40 chars, space-padded)
    memcpy(d->model, data + 24, 40);
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = '\0';

    pmm_free_pages(PHYS(data), 1);
    return true;
}

static spinlock_t nvme_lock = {0};

int nvme_read_sectors(nvme_drive_t *drive, uint64_t lba, uint32_t count, void *buf) {
    if (!drive || !drive->present || !buf || count == 0) return -1;

    uint32_t ss = drive->sector_size;

    for (uint32_t i = 0; i < count; i++) {
        paddr_t ppage = pmm_alloc_pages(1);
        if (!ppage) return -1;
        void *bounce = (void *)(uintptr_t)MMIO(ppage);
        memset(bounce, 0, ss);

        spinlock_acquire(&nvme_lock);
        nvme_sqe_t cmd = {0};
        cmd.opcode = NVME_NVM_READ;
        cmd.nsid   = drive->nsid;
        cmd.prp1   = ppage;   // already physical, page-aligned
        cmd.prp2   = 0;
        cmd.cdw10  = (uint32_t)((lba + i) & 0xFFFFFFFF);
        cmd.cdw11  = (uint32_t)((lba + i) >> 32);
        cmd.cdw12  = 0;       // NLB=0 means 1 sector

        uint16_t cid = nvme_next_cid(drive);
        cmd.cid = cid;
        nvme_io_submit(drive, &cmd);
        int r = nvme_io_poll(drive, cid);
        spinlock_release(&nvme_lock);

        if (r == 0)
            memcpy((uint8_t *)buf + (i * ss), bounce, ss);

        pmm_free_pages(ppage, 1);
        if (r != 0) return r;
    }
    return 0;
}

int nvme_write_sectors(nvme_drive_t *drive, uint64_t lba, uint32_t count, const void *buf) {
    if (!drive || !drive->present || !buf || count == 0) return -1;

    uint32_t ss = drive->sector_size;

    for (uint32_t i = 0; i < count; i++) {
        paddr_t ppage = pmm_alloc_pages(1);
        if (!ppage) return -1;
        void *bounce = (void *)(uintptr_t)MMIO(ppage);
        memcpy(bounce, (const uint8_t *)buf + (i * ss), ss);

        spinlock_acquire(&nvme_lock);
        nvme_sqe_t cmd = {0};
        cmd.opcode = NVME_NVM_WRITE;
        cmd.nsid   = drive->nsid;
        cmd.prp1   = ppage;
        cmd.prp2   = 0;
        cmd.cdw10  = (uint32_t)((lba + i) & 0xFFFFFFFF);
        cmd.cdw11  = (uint32_t)((lba + i) >> 32);
        cmd.cdw12  = 0;

        uint16_t cid = nvme_next_cid(drive);
        cmd.cid = cid;
        nvme_io_submit(drive, &cmd);
        int r = nvme_io_poll(drive, cid);
        spinlock_release(&nvme_lock);

        pmm_free_pages(ppage, 1);
        if (r != 0) return r;
    }
    return 0;
}

// block device stuff here
typedef struct {
    nvme_drive_t *drive;
    uint64_t      lba_start;
    uint64_t      sector_count;
    INode_t      *inode;
} nvme_blk_t;

static long nvme_blk_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    nvme_blk_t *blk = inode->internal_data;
    if (!blk || count == 0) return 0;

    uint32_t ss   = blk->drive->sector_size;
    size_t   max  = (size_t)(blk->sector_count * ss);
    if (offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    uint64_t abs_lba = blk->lba_start + offset / ss;
    size_t   skip    = offset % ss;
    size_t   total   = 0;

    uint8_t *tmp = kmalloc(ss);
    if (!tmp) return -1;

    while (total < count) {
        if (nvme_read_sectors(blk->drive, abs_lba, 1, tmp) < 0) {
            kfree(tmp, ss);
            return total > 0 ? (long)total : -1;
        }
        size_t src_off = (total == 0) ? skip : 0;
        size_t avail   = ss - src_off;
        size_t chunk   = count - total;
        if (chunk > avail) chunk = avail;
        memcpy((uint8_t *)buf + total, tmp + src_off, chunk);
        total += chunk;
        abs_lba++;
    }

    kfree(tmp, ss);
    return (long)total;
}

static long nvme_blk_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    nvme_blk_t *blk = inode->internal_data;
    if (!blk || count == 0) return 0;

    uint32_t ss   = blk->drive->sector_size;
    size_t   max  = (size_t)(blk->sector_count * ss);
    if (offset >= max) return 0;
    if (count > max - offset) count = max - offset;

    uint64_t abs_lba = blk->lba_start + offset / ss;
    size_t   skip    = offset % ss;
    size_t   total   = 0;

    uint8_t *tmp = kmalloc(ss);
    if (!tmp) return -1;

    while (total < count) {
        size_t dst_off = (total == 0) ? skip : 0;
        size_t avail   = ss - dst_off;
        size_t chunk   = count - total;
        if (chunk > avail) chunk = avail;

        // RMW if we're not writing a full sector
        if (dst_off != 0 || chunk < ss) {
            if (nvme_read_sectors(blk->drive, abs_lba, 1, tmp) < 0) {
                kfree(tmp, ss);
                return total > 0 ? (long)total : -1;
            }
        }
        memcpy(tmp + dst_off, (const uint8_t *)buf + total, chunk);
        if (nvme_write_sectors(blk->drive, abs_lba, 1, tmp) < 0) {
            kfree(tmp, ss);
            return total > 0 ? (long)total : -1;
        }
        total += chunk;
        abs_lba++;
    }

    kfree(tmp, ss);
    return (long)total;
}

static size_t nvme_blk_size(INode_t *inode) {
    nvme_blk_t *blk = inode->internal_data;
    return blk ? (size_t)(blk->sector_count * blk->drive->sector_size) : 0;
}

static INodeOps_t nvme_blk_ops = {
    .read  = nvme_blk_read,
    .write = nvme_blk_write,
    .size  = nvme_blk_size,
};

static nvme_drive_t nvme_drives[NVME_MAX_DRIVES];
static int          nvme_drive_count = 0;

static void nvme_register_drive(int drive_idx, nvme_drive_t *drive) {
    char dev_name[16];
    snprintf(dev_name, sizeof(dev_name), "nvme%d", drive_idx);

    nvme_blk_t *blk = kmalloc(sizeof(*blk));
    if (!blk) return;
    memset(blk, 0, sizeof(*blk));
    blk->drive        = drive;
    blk->lba_start    = 0;
    blk->sector_count = drive->sector_count;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(blk, sizeof(*blk)); return; }
    memset(inode, 0, sizeof(*inode));
    inode->type          = INODE_DEVICE;
    inode->ops           = &nvme_blk_ops;
    inode->internal_data = blk;
    inode->shared        = 1;
    blk->inode           = inode;

    if (device_register(inode, dev_name) < 0) {
        serial_printf(LOG_WARN "nvme: failed to register /dev/%s\n", dev_name);
        kfree(inode, sizeof(*inode));
        kfree(blk, sizeof(*blk));
        return;
    }
    serial_printf(LOG_OK "nvme: /dev/%s - %s (%llu sectors, %u bytes/sector)\n",
                  dev_name, drive->model, drive->sector_count, drive->sector_size);
}

static void nvme_register_part_cb(void *drive_obj, int drive_idx, int part_idx,
                                   uint64_t start, uint64_t count, uint8_t type) {
    nvme_drive_t *drive = (nvme_drive_t *)drive_obj;
    char part_name[16];
    snprintf(part_name, sizeof(part_name), "nvme%dp%d", drive_idx, part_idx + 1);

    nvme_blk_t *pblk = kmalloc(sizeof(*pblk));
    if (!pblk) return;
    memset(pblk, 0, sizeof(*pblk));
    pblk->drive        = drive;
    pblk->lba_start    = start;
    pblk->sector_count = count;

    INode_t *pinode = kmalloc(sizeof(*pinode));
    if (!pinode) { kfree(pblk, sizeof(*pblk)); return; }
    memset(pinode, 0, sizeof(*pinode));
    pinode->type          = INODE_DEVICE;
    pinode->ops           = &nvme_blk_ops;
    pinode->internal_data = pblk;
    pinode->shared        = 1;
    pblk->inode           = pinode;

    if (device_register(pinode, part_name) < 0) {
        serial_printf(LOG_WARN "nvme: failed to register /dev/%s\n", part_name);
        kfree(pinode, sizeof(*pinode));
        kfree(pblk, sizeof(*pblk));
        return;
    }
    serial_printf(LOG_OK "nvme: registered /dev/%s (lba=%llu sectors=%llu type=0x%02x)\n",
                  part_name, start, count, type);
}

// Wrapper for partition_probe
static int nvme_read_sectors_wrapper(void *drive, uint64_t lba, uint16_t count, void *buf) {
    return nvme_read_sectors((nvme_drive_t *)drive, lba, (uint32_t)count, buf);
}

nvme_drive_t *nvme_get_drive(int index) {
    if (index < 0 || index >= nvme_drive_count) return NULL;
    return nvme_drives[index].present ? &nvme_drives[index] : NULL;
}

void nvme_init(void) {
    memset(nvme_drives, 0, sizeof(nvme_drives));

    pci_device_t pci_dev;
    if (!pci_find_device(PCI_CLASS_STORAGE_NVME, PCI_SUBCLASS_NVME,
                         PCI_PROGIF_NVME, &pci_dev)) {
        serial_printf(LOG_INFO "nvme: no NVMe controller found\n");
        return;
    }

    serial_printf(LOG_OK "nvme: controller found - PCI %02x:%02x.%x "
                  "vendor=0x%04x device=0x%04x\n",
                  pci_dev.bus, pci_dev.device, pci_dev.function,
                  pci_dev.vendor_id, pci_dev.device_id);

    // Enable bus mastering + memory space
    uint16_t cmd = pci_read16(pci_dev.bus, pci_dev.device, pci_dev.function, PCI_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
    pci_write16(pci_dev.bus, pci_dev.device, pci_dev.function, PCI_COMMAND, cmd);

    // BAR0 is the 64-bit MMIO base for NVMe
    uint64_t bar0_lo  = pci_dev.bar[0] & ~0xFu;
    uint64_t bar0_hi  = pci_dev.bar[1];
    uint64_t bar0_phys = bar0_lo | (bar0_hi << 32);

    if (bar0_phys == 0) {
        serial_printf(LOG_ERROR "nvme: BAR0 is zero - controller not mapped\n");
        return;
    }

    // Map the MMIO region into the HHDM
    paddr_t  cr3       = read_cr3();
    uint64_t page_base = bar0_phys & ~(uint64_t)(PAGE_SIZE_4K - 1);
    uint64_t page_end  = (bar0_phys + 0x10000 + PAGE_SIZE_4K - 1)
                         & ~(uint64_t)(PAGE_SIZE_4K - 1);

    for (uint64_t p = page_base; p < page_end; p += PAGE_SIZE_4K) {
        uint64_t virt = p + nvme_hhdm_offset();
        uint64_t *pte = paging_get_page(cr3, virt, 1);
        if (pte) {
            *pte = (p & PADDR_ENTRY_MASK) | PTE_PRESENT | PTE_WRITABLE | PTE_NX;
            asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
        } else {
            serial_printf(LOG_ERROR "nvme: failed to map MMIO page 0x%llx\n", p);
            return;
        }
    }

    uint64_t mmio_base = MMIO(bar0_phys);
    serial_printf(LOG_INFO "nvme: BAR0 phys=0x%llx virt=0x%llx (mapped)\n",
                  bar0_phys, mmio_base);

    nvme_drive_t *drive = &nvme_drives[0];
    drive->mmio_base = mmio_base;

    if (!nvme_controller_init(drive)) {
        serial_printf(LOG_ERROR "nvme: controller init failed\n");
        return;
    }

    if (!nvme_identify_ctrl(drive)) {
        serial_printf(LOG_WARN "nvme: identify controller failed (non-fatal)\n");
    }

    // Namespace 1 is mandatory per the spec
    if (!nvme_identify_ns(drive, 1)) {
        serial_printf(LOG_ERROR "nvme: identify namespace 1 failed\n");
        return;
    }

    if (!nvme_set_num_queues(drive, 1)) {
        serial_printf(LOG_ERROR "nvme: queue negotiation failed\n");
        return;
    }

    if (!nvme_create_io_queues(drive)) {
        serial_printf(LOG_ERROR "nvme: IO queue creation failed\n");
        return;
    }

    drive->present = true;
    nvme_drive_count++;

    nvme_register_drive(0, drive);
    partition_probe(drive, 0, nvme_read_sectors_wrapper, nvme_register_part_cb);
}