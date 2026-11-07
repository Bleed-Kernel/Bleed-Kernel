#include "ext4_priv.h"

#include <string.h>

static uint32_t crc32c_table[256];
static bool     crc32c_ready;

uint32_t ext4_crc32c(uint32_t crc, const void *buf, size_t len) {
    if (!crc32c_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
            crc32c_table[i] = c;
        }
        crc32c_ready = true;
    }

    const uint8_t *p = buf;
    while (len--)
        crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint16_t ext4_crc16(uint16_t crc, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static uint32_t sb_csum(ext4_fs_t *fs) {
    return ext4_crc32c(~0u, &fs->sb, offsetof(ext4_superblock_t, s_checksum));
}

bool ext4_sb_csum_ok(ext4_fs_t *fs) {
    return !fs->csum || fs->sb.s_checksum == sb_csum(fs);
}

void ext4_sb_csum_set(ext4_fs_t *fs) {
    if (fs->csum)
        fs->sb.s_checksum = sb_csum(fs);
}

static uint16_t gd_csum(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd    = ext4_gd(fs, g);
    uint16_t   saved = gd->bg_checksum;
    uint32_t   le    = g;
    uint16_t   out;

    if (fs->csum) {
        gd->bg_checksum = 0;
        uint32_t c = ext4_crc32c(fs->csum_seed, &le, 4);
        out = (uint16_t)ext4_crc32c(c, gd, fs->desc_size);
    } else {
        // the crc16 one skips the checksum field instead of zeroing it
        size_t   at = offsetof(ext4_gd_t, bg_checksum);
        uint16_t c  = ext4_crc16(0xFFFF, fs->sb.s_uuid, sizeof(fs->sb.s_uuid));
        c = ext4_crc16(c, &le, 4);
        c = ext4_crc16(c, gd, at);
        at += 2;
        if (fs->desc_size > at)
            c = ext4_crc16(c, (uint8_t *)gd + at, fs->desc_size - at);
        out = c;
    }
    gd->bg_checksum = saved;
    return out;
}

bool ext4_gd_csum_ok(ext4_fs_t *fs, uint32_t g) {
    if (!fs->csum && !fs->gdt_csum)
        return true;
    return ext4_gd(fs, g)->bg_checksum == gd_csum(fs, g);
}

void ext4_gd_csum_set(ext4_fs_t *fs, uint32_t g) {
    if (fs->csum || fs->gdt_csum)
        ext4_gd(fs, g)->bg_checksum = gd_csum(fs, g);
}

void ext4_bitmap_csum_set(ext4_fs_t *fs, uint32_t g, const uint8_t *bm, bool inodes) {
    if (!fs->csum)
        return;

    ext4_gd_t *gd = ext4_gd(fs, g);
    if (inodes) {
        uint32_t c = ext4_crc32c(fs->csum_seed, bm, fs->inodes_per_group / 8);
        gd->bg_inode_bitmap_csum_lo = (uint16_t)c;
        if (fs->desc_size >= 64)
            gd->bg_inode_bitmap_csum_hi = (uint16_t)(c >> 16);
    } else {
        uint32_t c = ext4_crc32c(fs->csum_seed, bm, fs->blocks_per_group / 8);
        gd->bg_block_bitmap_csum_lo = (uint16_t)c;
        if (fs->desc_size >= 64)
            gd->bg_block_bitmap_csum_hi = (uint16_t)(c >> 16);
    }
}

static uint32_t inode_seed(ext4_fs_t *fs, uint32_t ino, uint32_t gen) {
    uint32_t c = ext4_crc32c(fs->csum_seed, &ino, 4);
    return ext4_crc32c(c, &gen, 4);
}

// raw is the whole inode_size record
void ext4_inode_csum_set(ext4_fs_t *fs, uint32_t ino, uint8_t *raw) {
    if (!fs->csum)
        return;

    ext4_disk_inode_t *di = (ext4_disk_inode_t *)raw;
    bool hi = fs->inode_size > EXT4_GOOD_OLD_INODE &&
              (size_t)EXT4_GOOD_OLD_INODE + di->i_extra_isize >= offsetof(ext4_disk_inode_t, i_checksum_hi) + 2;

    di->i_checksum_lo = 0;
    if (hi)
        di->i_checksum_hi = 0;

    uint32_t c = ext4_crc32c(inode_seed(fs, ino, di->i_generation), raw, fs->inode_size);
    di->i_checksum_lo = (uint16_t)c;
    if (hi)
        di->i_checksum_hi = (uint16_t)(c >> 16);
}

void ext4_extent_csum_set(ext4_inode_t *ei, uint8_t *block) {
    if (!ei->fs->csum)
        return;
    ext4_extent_header_t *h = (ext4_extent_header_t *)block;
    size_t at = sizeof(*h) + (size_t)h->eh_max * sizeof(ext4_extent_t);
    if (at + 4 > ei->fs->block_size)
        return;
    uint32_t c = ext4_crc32c(inode_seed(ei->fs, ei->ino, ei->disk.i_generation), block, at);
    memcpy(block + at, &c, 4);
}

void ext4_dirblock_csum_set(ext4_inode_t *ei, uint8_t *block) {
    if (!ei->fs->csum)
        return;
    size_t at = ei->fs->block_size - sizeof(ext4_dir_tail_t);
    ext4_dir_tail_t *t = (ext4_dir_tail_t *)(block + at);
    if (t->det_zero1 || t->det_rec_len != sizeof(*t) || t->det_ft != EXT4_DIR_TAIL_FT)
        return;
    t->det_checksum = ext4_crc32c(inode_seed(ei->fs, ei->ino, ei->disk.i_generation), block, at);
}

void ext4_xattr_csum_set(ext4_fs_t *fs, uint64_t blk, uint8_t *block) {
    if (!fs->csum)
        return;
    uint32_t zero = 0;
    memcpy(block + 0x10, &zero, 4);
    uint32_t c = ext4_crc32c(fs->csum_seed, &blk, 8);
    c = ext4_crc32c(c, block, fs->block_size);
    memcpy(block + 0x10, &c, 4);
}
