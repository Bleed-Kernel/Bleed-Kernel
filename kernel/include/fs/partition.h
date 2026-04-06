#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// MBR structures
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) part_mbr_entry_t;

typedef struct {
    uint8_t          bootstrap[446];
    part_mbr_entry_t partitions[4];
    uint16_t         signature;
} __attribute__((packed)) part_mbr_t;

#define PART_MBR_SIGNATURE 0xAA55

// GPT structures
typedef struct {
    char     signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_array_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) part_gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36]; // UTF-16
} __attribute__((packed)) part_gpt_entry_t;

// disk reading and partition registration callbacks
typedef int (*sector_reader_t)(void *drive_obj, uint64_t lba, uint16_t count, void *buf);
typedef void (*part_register_cb_t)(void *drive_obj, int drive_idx, int part_idx, uint64_t start, uint64_t count, uint8_t type);

void partition_probe(void *drive_obj, int drive_idx, sector_reader_t reader, part_register_cb_t reg_cb);