#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <fs/vfs.h>

// PCI identifiers for nvme
#define PCI_CLASS_STORAGE_NVME  0x01
#define PCI_SUBCLASS_NVME       0x08
#define PCI_PROGIF_NVME         0x02

// NVMe BAR0 register offsets
#define NVME_REG_CAP        0x00    // Controller Capabilities
#define NVME_REG_VS         0x08    // Version
#define NVME_REG_INTMS      0x0C    // Interrupt Mask Set
#define NVME_REG_INTMC      0x10    // Interrupt Mask Clear
#define NVME_REG_CC         0x14    // Controller Configuration
#define NVME_REG_CSTS       0x1C    // Controller Status
#define NVME_REG_AQA        0x24    // Admin Queue Attributes
#define NVME_REG_ASQ        0x28    // Admin Submission Queue Base
#define NVME_REG_ACQ        0x30    // Admin Completion Queue Base

// Controller Config Fields
#define NVME_CC_EN          (1u << 0)
#define NVME_CC_CSS_NVM     (0u << 4)   // NVM command set
#define NVME_CC_MPS_4K      (0u << 7)   // host page size = 4K
#define NVME_CC_AMS_RR      (0u << 11)  // round-robin arb
#define NVME_CC_IOSQES      (6u << 16)  // SQ entry size = 64B
#define NVME_CC_IOCQES      (4u << 20)  // CQ entry size = 16B

#define NVME_CSTS_RDY       (1u << 0)
#define NVME_CSTS_CFS       (1u << 1)   // fatal status

// Admin opcodes
#define NVME_ADM_DELETE_SQ  0x00
#define NVME_ADM_CREATE_SQ  0x01
#define NVME_ADM_DELETE_CQ  0x04
#define NVME_ADM_CREATE_CQ  0x05
#define NVME_ADM_IDENTIFY   0x06

// NVM opcodes
#define NVME_NVM_FLUSH      0x00
#define NVME_NVM_WRITE      0x01
#define NVME_NVM_READ       0x02

// Queue depths
#define NVME_ADMIN_QUEUE_DEPTH  16
#define NVME_IO_QUEUE_DEPTH     64

#define NVME_MAX_DRIVES 8
#define NVME_SECTOR_SIZE 512  // logical block size default overridden by identify

typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  fuse_psdt;
    uint16_t cid;           // command identifier
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

// NVMe completion queue entry
typedef struct __attribute__((packed)) {
    uint32_t dw0;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; // bit 0 is phase
} nvme_cqe_t;

// Identify Namespace data structure
typedef struct __attribute__((packed)) {
    uint64_t nsze;          // namespace size in LBAs
    uint64_t ncap;
    uint64_t nuse;
    uint8_t  nsfeat;
    uint8_t  nlbaf;
    uint8_t  flbas;         // formatted LBA size index
    uint8_t  rsvd[101 - 28];
    struct {
        uint16_t ms;
        uint8_t  lbads;     // log2 of LBA data size
        uint8_t  rp;
    } lbaf[16];
} nvme_identify_ns_t;

// Per-drive state
typedef struct {
    uint64_t  mmio_base;        // virtual address of BAR0
    uint32_t  doorbell_stride;  // in bytes, from CAP.DSTRD

    nvme_sqe_t *asq;
    nvme_cqe_t *acq;
    uint16_t    asq_tail;
    uint16_t    acq_head;
    uint8_t     acq_phase;
    uint16_t    next_cid;

    // IO queues
    nvme_sqe_t *iosq;
    nvme_cqe_t *iocq;
    uint16_t    iosq_tail;
    uint16_t    iocq_head;
    uint8_t     iocq_phase;

    uint32_t    nsid;           // first namespace id
    uint64_t    sector_count;
    uint32_t    sector_size;
    char        model[41];
    bool        present;
} nvme_drive_t;

// Public API
void nvme_init(void);
int  nvme_read_sectors (nvme_drive_t *drive, uint64_t lba, uint32_t count, void *buf);
int  nvme_write_sectors(nvme_drive_t *drive, uint64_t lba, uint32_t count, const void *buf);
nvme_drive_t *nvme_get_drive(int index);