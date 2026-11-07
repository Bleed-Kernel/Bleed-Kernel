#pragma once

#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>

#define EXT4_SUPER_MAGIC        0xEF53
#define EXT4_SUPERBLOCK_OFFSET  1024
#define EXT4_STATE_ERROR        0x0002  // s_state, errors were seen
#define EXT4_ROOT_INO           2
#define EXT4_GOOD_OLD_INODE     128
#define EXT4_N_BLOCKS           15

#define EXT4_S_IFMT    0xF000
#define EXT4_S_IFREG   0x8000
#define EXT4_S_IFDIR   0x4000
#define EXT4_S_IFLNK   0xA000

#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_SYMLINK  7

#define EXT4_COMPAT_HAS_JOURNAL         0x0004
#define EXT4_COMPAT_SPARSE_SUPER2       0x0200

#define EXT4_INCOMPAT_COMPRESSION       0x0001
#define EXT4_INCOMPAT_FILETYPE          0x0002
#define EXT4_INCOMPAT_RECOVER           0x0004
#define EXT4_INCOMPAT_JOURNAL_DEV       0x0008
#define EXT4_INCOMPAT_META_BG           0x0010
#define EXT4_INCOMPAT_EXTENTS           0x0040
#define EXT4_INCOMPAT_64BIT             0x0080
#define EXT4_INCOMPAT_MMP               0x0100
#define EXT4_INCOMPAT_FLEX_BG           0x0200
#define EXT4_INCOMPAT_EA_INODE          0x0400
#define EXT4_INCOMPAT_DIRDATA           0x1000
#define EXT4_INCOMPAT_CSUM_SEED         0x2000
#define EXT4_INCOMPAT_LARGEDIR          0x4000
#define EXT4_INCOMPAT_INLINE_DATA       0x8000
#define EXT4_INCOMPAT_ENCRYPT           0x10000
#define EXT4_INCOMPAT_CASEFOLD          0x20000

#define EXT4_RO_COMPAT_SPARSE_SUPER     0x0001
#define EXT4_RO_COMPAT_LARGE_FILE       0x0002
#define EXT4_RO_COMPAT_BTREE_DIR        0x0004
#define EXT4_RO_COMPAT_HUGE_FILE        0x0008
#define EXT4_RO_COMPAT_GDT_CSUM         0x0010
#define EXT4_RO_COMPAT_DIR_NLINK        0x0020
#define EXT4_RO_COMPAT_EXTRA_ISIZE      0x0040
#define EXT4_RO_COMPAT_BIGALLOC         0x0200
#define EXT4_RO_COMPAT_METADATA_CSUM    0x0400
#define EXT4_RO_COMPAT_ORPHAN_PRESENT   0x10000

// i_flags
#define EXT4_INDEX_FL       0x00001000  // htree directory
#define EXT4_HUGE_FILE_FL   0x00040000
#define EXT4_EXTENTS_FL     0x00080000
#define EXT4_INLINE_DATA_FL 0x10000000

// bg_flags
#define EXT4_BG_INODE_UNINIT    0x0001
#define EXT4_BG_BLOCK_UNINIT    0x0002
#define EXT4_BG_INODE_ZEROED    0x0004

#define EXT4_EXT_MAGIC          0xF30A
#define EXT4_EXT_INIT_MAX       32768   // longer than this is an unwritten extent
#define EXT4_DIR_TAIL_FT        0xDE
#define EXT4_XATTR_MAGIC        0xEA020000

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint8_t  s_encryption_level;
    uint8_t  s_reserved_pad;
    uint64_t s_kbytes_written;
    uint8_t  s_snapshot_and_errors[0x80];   // snapshot + first/last error fields, never touched
    uint8_t  s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_clusters;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint8_t  _pad[0x3FC - 0x274];
    uint32_t s_checksum;
} ext4_superblock_t;

_Static_assert(sizeof(ext4_superblock_t) == 1024, "ext4 superblock");
_Static_assert(__builtin_offsetof(ext4_superblock_t, s_desc_size) == 0xFE, "ext4 superblock");
_Static_assert(__builtin_offsetof(ext4_superblock_t, s_blocks_count_hi) == 0x150, "ext4 superblock");
_Static_assert(__builtin_offsetof(ext4_superblock_t, s_checksum_seed) == 0x270, "ext4 superblock");

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    // only there when s_desc_size is 64 or more
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} ext4_gd_t;

_Static_assert(sizeof(ext4_gd_t) == 64, "ext4 group descriptor");

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_version;
    uint32_t i_block[EXT4_N_BLOCKS];    // block map, extent root or a fast symlink
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint16_t i_blocks_high;
    uint16_t i_file_acl_high;
    uint16_t i_uid_high;
    uint16_t i_gid_high;
    uint16_t i_checksum_lo;
    uint16_t i_reserved;
    // past 128 bytes, each only real when i_extra_isize reaches it
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} ext4_disk_inode_t;

_Static_assert(sizeof(ext4_disk_inode_t) == 160, "ext4 inode");

typedef struct __attribute__((packed)) {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;      // 0 = entries are extents
    uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct __attribute__((packed)) {
    uint32_t ei_block;      // first logical block under this child
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;

typedef struct __attribute__((packed)) {
    uint32_t ee_block;
    uint16_t ee_len;        // over EXT4_EXT_INIT_MAX means unwritten
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext4_dirent_t;

// the fake dirent at the end of every directory block on a metadata_csum fs
typedef struct __attribute__((packed)) {
    uint32_t det_zero1;
    uint16_t det_rec_len;   // 12
    uint8_t  det_zero2;
    uint8_t  det_ft;        // EXT4_DIR_TAIL_FT
    uint32_t det_checksum;
} ext4_dir_tail_t;

// true when the ext2 driver cant be trusted with this superblock
bool ext4_probe(INode_t *dev_inode);
int ext4_mount(INode_t *dev_inode, INode_t **root);
