#pragma once

#include <fs/ext4/ext4.h>
#include <fs/vfs.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EXT4_MAP_CACHE      8
#define EXT4_BM_NONE        UINT32_MAX
#define EXT4_LINK_MAX       65000
#define EXT4_FAST_LINK_MAX  (EXT4_N_BLOCKS * 4)

typedef struct {
    INode_t  *dev;
    ext4_superblock_t sb;
    uint32_t  block_size;
    uint64_t  blocks_count;
    uint32_t  blocks_per_group;
    uint32_t  inodes_per_group;
    uint32_t  inode_size;
    uint32_t  desc_size;
    uint32_t  first_data_block;
    uint32_t  num_groups;
    uint32_t  total_inodes;
    uint32_t  csum_seed;
    bool      csum;         // metadata_csum, crc32c on all of it
    bool      gdt_csum;     // the older crc16 on group descriptors only
    bool      ro;

    uint8_t  *gdt;
    uint32_t  alloc_group;

    // block bitmap of the group being allocated from, goes out in ext4_alloc_flush
    uint8_t  *bm_buf;
    uint32_t  bm_group;
    uint32_t  bm_next;
    bool      bm_dirty;
    bool      sb_dirty;

    // extent nodes and indirect blocks
    struct {
        uint64_t  block;
        uint32_t  last_used;
        uint8_t  *data;
    } map_cache[EXT4_MAP_CACHE];
    uint32_t map_clock;
} ext4_fs_t;

typedef struct {
    ext4_fs_t        *fs;
    uint32_t          ino;
    ext4_disk_inode_t disk;
} ext4_inode_t;

// where a dirent sits, for removing or renaming it
typedef struct {
    uint32_t lblk;
    uint32_t off;
} ext4_dirpos_t;

// ext4_csum.c
uint32_t ext4_crc32c(uint32_t crc, const void *buf, size_t len);
uint16_t ext4_crc16(uint16_t crc, const void *buf, size_t len);
bool     ext4_sb_csum_ok(ext4_fs_t *fs);
void     ext4_sb_csum_set(ext4_fs_t *fs);
bool     ext4_gd_csum_ok(ext4_fs_t *fs, uint32_t g);
void     ext4_gd_csum_set(ext4_fs_t *fs, uint32_t g);
void     ext4_bitmap_csum_set(ext4_fs_t *fs, uint32_t g, const uint8_t *bm, bool inodes);
void     ext4_inode_csum_set(ext4_fs_t *fs, uint32_t ino, uint8_t *raw);
void     ext4_extent_csum_set(ext4_inode_t *ei, uint8_t *block);
void     ext4_dirblock_csum_set(ext4_inode_t *ei, uint8_t *block);
void     ext4_xattr_csum_set(ext4_fs_t *fs, uint64_t blk, uint8_t *block);

// ext4_group.c
int      ext4_read_bytes (ext4_fs_t *fs, uint64_t off, void *buf, size_t count);
int      ext4_write_bytes(ext4_fs_t *fs, uint64_t off, const void *buf, size_t count);
int      ext4_read_block (ext4_fs_t *fs, uint64_t block, void *buf);
int      ext4_write_block(ext4_fs_t *fs, uint64_t block, const void *buf);
uint8_t *ext4_cached_block(ext4_fs_t *fs, uint64_t block);

ext4_gd_t *ext4_gd(ext4_fs_t *fs, uint32_t g);
uint64_t ext4_gd_inode_table(ext4_fs_t *fs, uint32_t g);
uint32_t ext4_gd_free_blocks(ext4_fs_t *fs, uint32_t g);
int      ext4_gdt_load(ext4_fs_t *fs);
void     ext4_sb_write(ext4_fs_t *fs);

uint64_t ext4_alloc_block(ext4_fs_t *fs, uint64_t goal, bool zero);
void     ext4_free_blocks(ext4_fs_t *fs, uint64_t blk, uint64_t count);
void     ext4_alloc_flush(ext4_fs_t *fs);
uint32_t ext4_alloc_inode(ext4_fs_t *fs, uint32_t near_ino, bool dir);
void     ext4_free_inode(ext4_fs_t *fs, uint32_t ino, bool dir);

int      ext4_read_inode (ext4_fs_t *fs, uint32_t ino, ext4_disk_inode_t *out);
int      ext4_write_inode(ext4_fs_t *fs, uint32_t ino, const ext4_disk_inode_t *in, bool fresh);

// ext4_map.c
int      ext4_map(ext4_inode_t *ei, uint32_t lblk, uint64_t *pblk, uint32_t *len, bool *unwritten);
int      ext4_map_set(ext4_inode_t *ei, uint32_t lblk, uint64_t pblk, uint32_t count);
int      ext4_map_mark_written(ext4_inode_t *ei, uint32_t lblk);
int      ext4_map_truncate(ext4_inode_t *ei, uint32_t from);
void     ext4_map_init(ext4_inode_t *ei);
void     ext4_iblocks_add(ext4_inode_t *ei, int64_t fs_blocks);

// ext4_journal.c
int      ext4_journal_replay(ext4_fs_t *fs);

// ext4_dir.c
typedef bool (*ext4_dirent_cb)(const ext4_dirent_t *de, const char *name, ext4_dirpos_t pos, void *ctx);

void     ext4_dir_iterate(ext4_inode_t *dir, ext4_dirent_cb cb, void *ctx);
int      ext4_dir_find(ext4_inode_t *dir, const char *name, size_t len, uint32_t *ino, ext4_dirpos_t *pos);
int      ext4_dir_nth(ext4_inode_t *dir, size_t index, char *name, uint32_t *ino);
bool     ext4_dir_empty(ext4_inode_t *dir);
int      ext4_dir_add(ext4_inode_t *dir, const char *name, size_t len, uint32_t ino, uint8_t ft);
int      ext4_dir_remove(ext4_inode_t *dir, ext4_dirpos_t pos);
int      ext4_dir_rename(ext4_inode_t *dir, ext4_dirpos_t pos, const char *name, size_t len);
int      ext4_dir_init(ext4_inode_t *dir, uint32_t parent_ino);

// inode field helpers
static inline uint64_t ext4_isize(const ext4_disk_inode_t *di) {
    return di->i_size_lo | ((uint64_t)di->i_size_high << 32);
}

static inline void ext4_isize_set(ext4_disk_inode_t *di, uint64_t size) {
    di->i_size_lo   = (uint32_t)size;
    di->i_size_high = (uint32_t)(size >> 32);
}

static inline uint64_t ext4_file_acl(const ext4_disk_inode_t *di) {
    return di->i_file_acl_lo | ((uint64_t)di->i_file_acl_high << 32);
}

static inline bool ext4_is_dir(const ext4_disk_inode_t *di) {
    return (di->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
}

// a symlink whose target lives in i_block
static inline bool ext4_fast_link(const ext4_fs_t *fs, const ext4_disk_inode_t *di) {
    if ((di->i_mode & EXT4_S_IFMT) != EXT4_S_IFLNK || ext4_isize(di) >= EXT4_FAST_LINK_MAX)
        return false;
    uint64_t blocks = di->i_blocks_lo | ((uint64_t)di->i_blocks_high << 32);
    uint64_t acl    = ext4_file_acl(di) ? fs->block_size / 512 : 0;
    return blocks <= acl;
}
