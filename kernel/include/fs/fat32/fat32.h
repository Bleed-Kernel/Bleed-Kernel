#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <fs/vfs.h>
#include <devices/type/blk_device.h>

// bios params block
typedef struct __attribute__((packed)) fat32_bpb {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} fat32_bpb_t;

// dir entry
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F

#define FAT32_ENTRY_FREE      0xE5
#define FAT32_ENTRY_END       0x00

#define FAT32_CLUSTER_FREE    0x00000000
#define FAT32_CLUSTER_BAD     0x0FFFFFF7
#define FAT32_CLUSTER_EOC     0x0FFFFFF8
#define FAT32_CLUSTER_MASK    0x0FFFFFFF

typedef struct __attribute__((packed)) fat32_dirent {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat32_dirent_t;

//lfn entry
typedef struct __attribute__((packed)) fat32_lfn {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t first_cluster; // 0
    uint16_t name3[2];
} fat32_lfn_t;

// fs state
typedef struct fat32_fs {
    blk_device_t *blk;

    // basically just a compounded bpb
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t fat_start_lba;
    uint32_t fat_size_sectors;
    uint32_t data_start_lba;
    uint32_t root_cluster;
    uint32_t total_clusters;
} fat32_fs_t;

// fat32 inode intenal data section
typedef struct fat32_inode {
    fat32_fs_t  *fs;
    uint32_t     first_cluster;
    uint32_t     file_size;

    uint32_t     dirent_cluster;
    uint32_t     dirent_offset;
} fat32_inode_t;

int fat32_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result);
long fat32_read(INode_t *inode, void *buf, size_t count, size_t offset);
long fat32_write(INode_t *inode, const void *buf, size_t count, size_t offset);
int fat32_readdir(INode_t *dir, size_t index, INode_t **result);
int fat32_create(INode_t *parent, const char *name, size_t namelen, INode_t **result, inode_type node_type);
int fat32_unlink(INode_t *dir, const char *name, size_t namelen);
int fat32_rename(INode_t *dir, const char *oldname, size_t oldlen, const char *newname, size_t newlen);
int fat32_truncate(INode_t *inode, size_t new_size);
size_t fat32_size(INode_t *inode);
void fat32_drop(INode_t *inode);

// mount fat32 file system from block device
int fat32_mount(blk_device_t *blk, INode_t **root);

// fs descriptor
extern const filesystem fat32_filesystem;