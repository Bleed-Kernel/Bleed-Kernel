#pragma once

#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

// 512 boot sector
typedef struct __attribute__((packed)) {
    uint8_t  jump_boot[3];
    uint8_t  fs_name[8];                 // "EXFAT   "
    uint8_t  must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_dir;
    uint32_t volume_serial_number;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  number_of_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
    uint8_t  boot_code[390];
    uint16_t boot_signature;             // 0xAA55 defined below
} exfat_boot_sector_t;

#define EXFAT_BOOT_SIGNATURE 0xAA55

#define EXFAT_ENTRY_END           0x00  // unused/end of directory marker  
#define EXFAT_ENTRY_INUSE_BIT     0x80  // set => entry in use and clear => deleted 
#define EXFAT_ENTRY_TYPECODE_MASK 0x7F

#define EXFAT_TYPE_BITMAP   0x01  // + InUse = 0x81
#define EXFAT_TYPE_UPCASE   0x02  // + InUse = 0x82 
#define EXFAT_TYPE_LABEL    0x03  // + InUse = 0x83 
#define EXFAT_TYPE_FILE     0x05  // + InUse = 0x85 
#define EXFAT_TYPE_STREAM   0x40  // + InUse = 0xC0 
#define EXFAT_TYPE_NAME     0x41  // + InUse = 0xC1 

#define EXFAT_ENTRY_BITMAP  (EXFAT_TYPE_BITMAP | EXFAT_ENTRY_INUSE_BIT)
#define EXFAT_ENTRY_UPCASE  (EXFAT_TYPE_UPCASE | EXFAT_ENTRY_INUSE_BIT)
#define EXFAT_ENTRY_LABEL   (EXFAT_TYPE_LABEL  | EXFAT_ENTRY_INUSE_BIT)
#define EXFAT_ENTRY_FILE    (EXFAT_TYPE_FILE   | EXFAT_ENTRY_INUSE_BIT)
#define EXFAT_ENTRY_STREAM  (EXFAT_TYPE_STREAM | EXFAT_ENTRY_INUSE_BIT)
#define EXFAT_ENTRY_NAME    (EXFAT_TYPE_NAME   | EXFAT_ENTRY_INUSE_BIT)

// generic 32 byte slot, used to peek at EntryType
typedef struct __attribute__((packed)) {
    uint8_t entry_type;
    uint8_t raw[31];
} exfat_raw_dentry_t;

// File Directory Entry  primary entry of a file/dir entry set 
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;          // 0x85 
    uint8_t  secondary_count;     // number of secondary entries following 
    uint16_t set_checksum;
    uint16_t file_attributes;
    uint16_t reserved1;
    uint32_t create_timestamp;
    uint32_t last_modified_timestamp;
    uint32_t last_accessed_timestamp;
    uint8_t  create_10ms;
    uint8_t  last_modified_10ms;
    uint8_t  create_utc_offset;
    uint8_t  last_modified_utc_offset;
    uint8_t  last_accessed_utc_offset;
    uint8_t  reserved2[7];
} exfat_file_dentry_t;

#define EXFAT_ATTR_READ_ONLY 0x0001
#define EXFAT_ATTR_HIDDEN    0x0002
#define EXFAT_ATTR_SYSTEM    0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE   0x0020

// Stream Extension Entry mandatory secondary entry, holds size/location 
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;          // 0xC0 
    uint8_t  flags;               // bit0 AllocationPossible, bit1 NoFatChain 
    uint8_t  reserved1;
    uint8_t  name_length;         // in UTF16 code units 
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_stream_dentry_t;

#define EXFAT_FLAG_ALLOC_POSSIBLE 0x01
#define EXFAT_FLAG_NO_FAT_CHAIN   0x02

// File Name Entry secondary entry, 15 UTF16 code units per slot 
#define EXFAT_NAME_CHARS_PER_ENTRY 15
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;          // 0xC1 
    uint8_t  flags;
    uint16_t name[EXFAT_NAME_CHARS_PER_ENTRY];
} exfat_name_dentry_t;

// Allocation Bitmap Entry  lives in the root directory 
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;          // 0x81 
    uint8_t  bitmap_flags;
    uint8_t  reserved[18];
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_bitmap_dentry_t;

// Upcase Table Entry  lives in the root directory 
typedef struct __attribute__((packed)) {
    uint8_t  entry_type;          // 0x82 
    uint8_t  reserved1[3];
    uint32_t table_checksum;
    uint8_t  reserved2[12];
    uint32_t first_cluster;
    uint64_t data_length;
} exfat_upcase_dentry_t;

// FAT chain special values
#define EXFAT_CLUSTER_FREE 0x00000000u
#define EXFAT_CLUSTER_BAD  0xFFFFFFF7u
#define EXFAT_CLUSTER_EOF  0xFFFFFFFFu
#define EXFAT_FIRST_DATA_CLUSTER 2u

typedef struct {
    INode_t *dev;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;

    uint32_t fat_start_lba;
    uint32_t fat_length_sectors;

    uint32_t cluster_heap_lba;
    uint32_t cluster_count;

    uint32_t root_cluster;

    uint32_t bitmap_cluster;
    uint64_t bitmap_size_bytes;

    uint16_t *upcase_table;
    size_t    upcase_count;
} exfat_fs_t;

// perinode private data 
typedef struct {
    exfat_fs_t *fs;

    uint32_t first_cluster;
    uint64_t file_size;
    uint64_t valid_data_length;
    bool     no_fat_chain;        // true => cluster run is contiguous, ignore FAT 
    uint16_t attributes;

    uint32_t dirent_cluster;
    uint32_t dirent_offset;
    uint8_t  secondary_count;
} exfat_inode_t;

int exfat_mount(INode_t *dev_inode, INode_t **root);