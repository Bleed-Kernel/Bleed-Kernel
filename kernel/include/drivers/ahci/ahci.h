#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// AHCI Port Count
#define AHCI_MAX_PORTS      32
#define AHCI_MAX_DRIVES     32   // one per port
#define AHCI_MAX_PARTITIONS  4

// FIS Count
#define FIS_TYPE_REG_H2D    0x27
#define FIS_TYPE_REG_D2H    0x34
#define FIS_TYPE_DMA_SETUP  0x41
#define FIS_TYPE_PIO_SETUP  0x5F
#define FIS_TYPE_DEV_BITS   0xA1

// ATA commands
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35
#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_IDENTIFY_ATAPI 0xA1
#define ATA_DEV_BUSY          0x80
#define ATA_DEV_DRQ           0x08

// ACHI GHC register offsets
#define AHCI_GHC_CAP        0x00  // host capabilities
#define AHCI_GHC_GHC        0x04  // global host contro
#define AHCI_GHC_IS         0x08  // interrupt status
#define AHCI_GHC_PI         0x0C  // ports implemented
#define AHCI_GHC_VS         0x10  // version

#define AHCI_GHC_ENABLE     (1u << 31) 
#define AHCI_GHC_RESET      (1u << 0)

// port specific register offsets
#define AHCI_PORT_CLB       0x00    // command list base (low) 
#define AHCI_PORT_CLBU      0x04    // command list base (high) 
#define AHCI_PORT_FB        0x08    // FIS base (low) 
#define AHCI_PORT_FBU      0x0C     // FIS base (high) 
#define AHCI_PORT_IS        0x10    // interrupt status 
#define AHCI_PORT_IE        0x14    // interrupt enable 
#define AHCI_PORT_CMD       0x18    // command and status 
#define AHCI_PORT_TFD       0x20    // task file data 
#define AHCI_PORT_SIG       0x24    // signature 
#define AHCI_PORT_SSTS      0x28    // SATA status 
#define AHCI_PORT_SCTL      0x2C    // SATA control 
#define AHCI_PORT_SERR      0x30    // SATA error 
#define AHCI_PORT_SACT      0x34    // SATA active 
#define AHCI_PORT_CI        0x38    // command issue 

// port cmd bits
#define AHCI_PORT_CMD_ST    (1u <<  0)  // start 
#define AHCI_PORT_CMD_FRE   (1u <<  4)  // FIS receive enable 
#define AHCI_PORT_CMD_FR    (1u << 14)  // FIS receive running 
#define AHCI_PORT_CMD_CR    (1u << 15)  // command list running 

// PORT_SSTS detection 
#define AHCI_SSTS_DET_MASK  0x0F
#define AHCI_SSTS_DET_PRESENT 0x03  // device present and PHY comm established 

// Port signatures for what hardware it is
#define AHCI_SIG_ATA        0x00000101  // SATA disk 
#define AHCI_SIG_ATAPI      0xEB140101  // ATAPI signature

// prdt entry
typedef struct __attribute__((packed)) ahci_prdt_entry {
    uint32_t dba;        // low data base address
    uint32_t dbau;       // high data base address
    uint32_t reserved;
    uint32_t dbc;        // byte count
} ahci_prdt_entry_t;

#define AHCI_PRDT_ENTRIES   8

typedef struct __attribute__((packed)) ahci_cmd_table {
    uint8_t          cfis[64];   // command FIS 
    uint8_t          acmd[16];   // ATAPI command 
    uint8_t          reserved[48];
    ahci_prdt_entry_t prdt[AHCI_PRDT_ENTRIES];
} ahci_cmd_table_t;

// cmd header
typedef struct __attribute__((packed)) ahci_cmd_header {
    uint16_t flags;      // bit 0-4: FIS length in DWORDs, bit 6: write, bit 5: ATAPI
    uint16_t prdtl;      // PRDT entry count / len
    uint32_t prdbc;      // PRDT byte count transferred 
    uint32_t ctba;       // low command table base address
    uint32_t ctbau;      // high command table base address
    uint32_t reserved[4];
} ahci_cmd_header_t;

typedef struct __attribute__((packed)) fis_reg_h2d {
    uint8_t  fis_type;   // FIS_TYPE_REG_H2D 
    uint8_t  pmport_c;   // bit 7: update command register 
    uint8_t  command;
    uint8_t  featurel;

    uint8_t  lba0, lba1, lba2;
    uint8_t  device;

    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;

    uint8_t  countl, counth;
    uint8_t  icc;
    uint8_t  control;

    uint8_t  reserved[4];
} fis_reg_h2d_t;

// drive descriptor
typedef struct ahci_drive {
    bool     present;
    uint8_t  port;
    uint64_t abar;        // vaddr of AHCI MMIO base 
    uint64_t sector_count;
    char     model[41];

    // Per-port DMA buffers
    ahci_cmd_header_t *cmd_list;   // 32 command headers 1 KB aligned 
    uint8_t           *fis_buf;    // FIS buffer 256-byte aligned 
    ahci_cmd_table_t  *cmd_table;  // command table for slot 0 

    struct {
        bool     present;
        uint8_t  type;
        uint32_t lba_start;
        uint32_t sector_count;
    } partitions[AHCI_MAX_PARTITIONS];
} ahci_drive_t;

// Detect all AHCI drives reads MBR then registers them
void ahci_init(void);

// Read "count" sectors starting at "lba" into "buf"
int ahci_read_sectors(ahci_drive_t *drive, uint64_t lba,
                      uint16_t count, void *buf);

//Write "count" sectors starting at "lba" from "buf"
int ahci_write_sectors(ahci_drive_t *drive, uint64_t lba,
                       uint16_t count, const void *buf);

// Return drive descriptor by index 
ahci_drive_t *ahci_get_drive(int index);