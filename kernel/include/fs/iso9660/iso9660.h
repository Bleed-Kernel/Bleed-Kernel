#pragma once

#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

#define ISO_SECTOR          2048
#define ISO_VD_LBA          16          // volume descriptors start here cause theres system descriptors in the way
#define ISO_VD_MAX          32          // how many we look through for the primary one
#define ISO_VD_PRIMARY      1
#define ISO_VD_END          255
#define ISO_MAGIC           "CD001"
#define ISO_MAGIC_OFF       (ISO_VD_LBA * ISO_SECTOR + 1)

#define ISO_FLAG_DIR        0x02
#define ISO_FLAG_MULTI      0x80        // file carries on in the next record, only for files past 4G

// one record in a directory extent, the name follows and then the system use area (rock ridge)
typedef struct __attribute__((packed)) {
    uint8_t  length;
    uint8_t  ext_attr_length;
    uint32_t extent;
    uint32_t extent_be;
    uint32_t size;
    uint32_t size_be;
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  unit_size;
    uint8_t  gap_size;
    uint16_t volume_seq;
    uint16_t volume_seq_be;
    uint8_t  name_len;
    char     name[];
} iso_dirent_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    char     id[5];
    uint8_t  version;
    uint8_t  unused0;
    char     system_id[32];
    char     volume_id[32];
    uint8_t  unused1[8];
    uint32_t space;
    uint32_t space_be;
    uint8_t  unused2[32];
    uint32_t set_size;
    uint32_t seq;
    uint16_t block_size;
    uint16_t block_size_be;
    uint32_t path_table_size[2];
    uint32_t path_tables[4];
    uint8_t  root[34];
} iso_pvd_t;

bool iso9660_probe(INode_t *dev_inode);

// mount the iso9660 volume on dev_inode
int iso9660_mount(INode_t *dev_inode, INode_t **root);
