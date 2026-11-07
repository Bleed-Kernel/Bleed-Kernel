// block io, group descriptors, bitmaps and the inode table

#include "ext4_priv.h"

#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <user/errno.h>

int ext4_read_bytes(ext4_fs_t *fs, uint64_t off, void *buf, size_t count) {
    long r = inode_read(fs->dev, buf, count, off);
    return (r == (long)count) ? 0 : -EIO;
}

int ext4_write_bytes(ext4_fs_t *fs, uint64_t off, const void *buf, size_t count) {
    if (fs->ro)
        return -EROFS;
    for (int i = 0; i < EXT4_MAP_CACHE; i++) {
        uint64_t at = fs->map_cache[i].block * fs->block_size;
        if (fs->map_cache[i].block && off < at + fs->block_size && at < off + count)
            fs->map_cache[i].block = 0;
    }
    long r = inode_write(fs->dev, buf, count, off);
    return (r == (long)count) ? 0 : -EIO;
}

int ext4_read_block(ext4_fs_t *fs, uint64_t block, void *buf) {
    if (block >= fs->blocks_count)
        return -EIO;
    return ext4_read_bytes(fs, block * fs->block_size, buf, fs->block_size);
}

int ext4_write_block(ext4_fs_t *fs, uint64_t block, const void *buf) {
    if (!block || block >= fs->blocks_count)
        return -EIO;
    return ext4_write_bytes(fs, block * fs->block_size, buf, fs->block_size);
}

// read only copy of a metadata block, good until the next call
uint8_t *ext4_cached_block(ext4_fs_t *fs, uint64_t block) {
    if (!block || block >= fs->blocks_count)
        return NULL;

    int pick = 0;
    for (int i = 0; i < EXT4_MAP_CACHE; i++) {
        if (fs->map_cache[i].block == block) {
            fs->map_cache[i].last_used = ++fs->map_clock;
            return fs->map_cache[i].data;
        }
        if (fs->map_cache[i].last_used < fs->map_cache[pick].last_used)
            pick = i;
    }

    if (!fs->map_cache[pick].data && !(fs->map_cache[pick].data = kmalloc(fs->block_size)))
        return NULL;
    fs->map_cache[pick].block = 0;
    if (ext4_read_block(fs, block, fs->map_cache[pick].data) < 0)
        return NULL;
    fs->map_cache[pick].block     = block;
    fs->map_cache[pick].last_used = ++fs->map_clock;
    return fs->map_cache[pick].data;
}

// group descriptors
#define WIDE(fs) ((fs)->desc_size >= 64)

ext4_gd_t *ext4_gd(ext4_fs_t *fs, uint32_t g) {
    return (ext4_gd_t *)(fs->gdt + (size_t)g * fs->desc_size);
}

static uint64_t gd_block_bitmap(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    return gd->bg_block_bitmap_lo | (WIDE(fs) ? (uint64_t)gd->bg_block_bitmap_hi << 32 : 0);
}

static uint64_t gd_inode_bitmap(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    return gd->bg_inode_bitmap_lo | (WIDE(fs) ? (uint64_t)gd->bg_inode_bitmap_hi << 32 : 0);
}

uint64_t ext4_gd_inode_table(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    return gd->bg_inode_table_lo | (WIDE(fs) ? (uint64_t)gd->bg_inode_table_hi << 32 : 0);
}

uint32_t ext4_gd_free_blocks(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    return gd->bg_free_blocks_count_lo | (WIDE(fs) ? (uint32_t)gd->bg_free_blocks_count_hi << 16 : 0);
}

static void gd_free_blocks_add(ext4_fs_t *fs, uint32_t g, int32_t d) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    uint32_t   v  = ext4_gd_free_blocks(fs, g) + d;
    gd->bg_free_blocks_count_lo = (uint16_t)v;
    if (WIDE(fs))
        gd->bg_free_blocks_count_hi = (uint16_t)(v >> 16);
}

static uint32_t gd_free_inodes(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    return gd->bg_free_inodes_count_lo | (WIDE(fs) ? (uint32_t)gd->bg_free_inodes_count_hi << 16 : 0);
}

static void gd_free_inodes_add(ext4_fs_t *fs, uint32_t g, int32_t d) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    uint32_t   v  = gd_free_inodes(fs, g) + d;
    gd->bg_free_inodes_count_lo = (uint16_t)v;
    if (WIDE(fs))
        gd->bg_free_inodes_count_hi = (uint16_t)(v >> 16);
}

static void gd_used_dirs_add(ext4_fs_t *fs, uint32_t g, int32_t d) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    uint32_t   v  = gd->bg_used_dirs_count_lo | (WIDE(fs) ? (uint32_t)gd->bg_used_dirs_count_hi << 16 : 0);
    if (d < 0 && v == 0)
        return;
    v += d;
    gd->bg_used_dirs_count_lo = (uint16_t)v;
    if (WIDE(fs))
        gd->bg_used_dirs_count_hi = (uint16_t)(v >> 16);
}

static uint32_t gd_itable_unused(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    return gd->bg_itable_unused_lo | (WIDE(fs) ? (uint32_t)gd->bg_itable_unused_hi << 16 : 0);
}

static void gd_itable_unused_set(ext4_fs_t *fs, uint32_t g, uint32_t v) {
    ext4_gd_t *gd = ext4_gd(fs, g);
    gd->bg_itable_unused_lo = (uint16_t)v;
    if (WIDE(fs))
        gd->bg_itable_unused_hi = (uint16_t)(v >> 16);
}

static bool uninit_flags(ext4_fs_t *fs) {
    return fs->csum || fs->gdt_csum;
}

static bool group_has_super(ext4_fs_t *fs, uint32_t g) {
    if (g == 0)
        return true;
    if (fs->sb.s_feature_compat & EXT4_COMPAT_SPARSE_SUPER2)
        return g == fs->sb.s_backup_bgs[0] || g == fs->sb.s_backup_bgs[1];
    if (!(fs->sb.s_feature_ro_compat & EXT4_RO_COMPAT_SPARSE_SUPER) || g == 1)
        return true;
    if (!(g & 1))
        return false;
    for (uint64_t p = 3; p <= 7; p += 2) {
        uint64_t n = p;
        while (n < g)
            n *= p;
        if (n == g)
            return true;
    }
    return false;
}

static uint64_t group_first_block(ext4_fs_t *fs, uint32_t g) {
    return fs->first_data_block + (uint64_t)g * fs->blocks_per_group;
}

static uint32_t blocks_in_group(ext4_fs_t *fs, uint32_t g) {
    if (g == fs->num_groups - 1)
        return (uint32_t)(fs->blocks_count - group_first_block(fs, g));
    return fs->blocks_per_group;
}

static bool meta_bg_group(ext4_fs_t *fs, uint32_t g) {
    uint32_t dpb = fs->block_size / fs->desc_size;
    return (fs->sb.s_feature_incompat & EXT4_INCOMPAT_META_BG) && g / dpb >= fs->sb.s_first_meta_bg;
}

// meta_bg keeps each block of descriptors in the first group it describes
static uint64_t gd_offset(ext4_fs_t *fs, uint32_t g) {
    uint32_t dpb = fs->block_size / fs->desc_size;
    if (!meta_bg_group(fs, g))
        return (uint64_t)(fs->first_data_block + 1) * fs->block_size + (uint64_t)g * fs->desc_size;

    uint32_t first = g / dpb * dpb;
    uint64_t blk   = group_first_block(fs, first) + (group_has_super(fs, first) ? 1 : 0);
    return blk * fs->block_size + (uint64_t)(g % dpb) * fs->desc_size;
}

int ext4_gdt_load(ext4_fs_t *fs) {
    uint32_t dpb = fs->block_size / fs->desc_size;

    fs->gdt = kmalloc((size_t)fs->num_groups * fs->desc_size);
    if (!fs->gdt)
        return -ENOMEM;

    for (uint32_t g = 0; g < fs->num_groups; g += dpb) {
        uint32_t n = fs->num_groups - g < dpb ? fs->num_groups - g : dpb;
        if (ext4_read_bytes(fs, gd_offset(fs, g), ext4_gd(fs, g), (size_t)n * fs->desc_size) < 0)
            return -EIO;
    }
    return 0;
}

static void gd_write(ext4_fs_t *fs, uint32_t g) {
    ext4_gd_csum_set(fs, g);
    ext4_write_bytes(fs, gd_offset(fs, g), ext4_gd(fs, g), fs->desc_size);
}

void ext4_sb_write(ext4_fs_t *fs) {
    ext4_sb_csum_set(fs);
    ext4_write_bytes(fs, EXT4_SUPERBLOCK_OFFSET, &fs->sb, sizeof(fs->sb));
}

static void sb_free_blocks_add(ext4_fs_t *fs, int64_t d) {
    uint64_t v = fs->sb.s_free_blocks_count_lo;
    if (fs->sb.s_feature_incompat & EXT4_INCOMPAT_64BIT)
        v |= (uint64_t)fs->sb.s_free_blocks_count_hi << 32;
    v += d;
    fs->sb.s_free_blocks_count_lo = (uint32_t)v;
    if (fs->sb.s_feature_incompat & EXT4_INCOMPAT_64BIT)
        fs->sb.s_free_blocks_count_hi = (uint32_t)(v >> 32);
    fs->sb_dirty = true;
}

// bitmaps
static inline bool bit_test(const uint8_t *bm, uint32_t bit) { return bm[bit / 8] & (1u << (bit % 8)); }
static inline void bit_set (uint8_t *bm, uint32_t bit)       { bm[bit / 8] |= (uint8_t)(1u << (bit % 8)); }
static inline void bit_clr (uint8_t *bm, uint32_t bit)       { bm[bit / 8] &= (uint8_t)~(1u << (bit % 8)); }

// a group mkfs never touched has no bitmap on disk, linux works out whats in it like this
static void block_bitmap_init(ext4_fs_t *fs, uint32_t g, uint8_t *bm) {
    memset(bm, 0, fs->block_size);

    uint32_t dpb  = fs->block_size / fs->desc_size;
    uint32_t meta = group_has_super(fs, g) ? 1 : 0;
    if (!meta_bg_group(fs, g)) {
        if (meta) {
            bool mbg = fs->sb.s_feature_incompat & EXT4_INCOMPAT_META_BG;
            meta += mbg ? fs->sb.s_first_meta_bg : (fs->num_groups + dpb - 1) / dpb;
            meta += fs->sb.s_reserved_gdt_blocks;
        }
    } else {
        uint32_t i = g % dpb;
        if (i == 0 || i == 1 || i == dpb - 1)
            meta++;
    }
    for (uint32_t i = 0; i < meta && i < fs->blocks_per_group; i++)
        bit_set(bm, i);

    // its own bitmaps and inode table, unless flex_bg put them in another group
    uint64_t start = group_first_block(fs, g);
    uint64_t itb   = ((uint64_t)fs->inodes_per_group * fs->inode_size + fs->block_size - 1) / fs->block_size;
    uint64_t own[2] = { gd_block_bitmap(fs, g), gd_inode_bitmap(fs, g) };
    for (int i = 0; i < 2; i++) {
        if (own[i] >= start && own[i] - start < fs->blocks_per_group)
            bit_set(bm, (uint32_t)(own[i] - start));
    }
    uint64_t table = ext4_gd_inode_table(fs, g);
    for (uint64_t b = table; b < table + itb; b++) {
        if (b >= start && b - start < fs->blocks_per_group)
            bit_set(bm, (uint32_t)(b - start));
    }

    for (uint32_t i = blocks_in_group(fs, g); i < fs->block_size * 8; i++)
        bit_set(bm, i);
}

void ext4_alloc_flush(ext4_fs_t *fs) {
    if (fs->bm_group != EXT4_BM_NONE && fs->bm_dirty) {
        ext4_bitmap_csum_set(fs, fs->bm_group, fs->bm_buf, false);
        ext4_write_block(fs, gd_block_bitmap(fs, fs->bm_group), fs->bm_buf);
        gd_write(fs, fs->bm_group);
        fs->bm_dirty = false;
    }
    if (fs->sb_dirty) {
        ext4_sb_write(fs);
        fs->sb_dirty = false;
    }
}

static bool bm_load(ext4_fs_t *fs, uint32_t g) {
    if (fs->bm_group == g)
        return true;
    ext4_alloc_flush(fs);
    fs->bm_group = EXT4_BM_NONE;

    if (!fs->bm_buf && !(fs->bm_buf = kmalloc(fs->block_size)))
        return false;

    ext4_gd_t *gd = ext4_gd(fs, g);
    if (uninit_flags(fs) && (gd->bg_flags & EXT4_BG_BLOCK_UNINIT)) {
        block_bitmap_init(fs, g, fs->bm_buf);
        gd->bg_flags &= ~EXT4_BG_BLOCK_UNINIT;
        fs->bm_group = g;
        fs->bm_dirty = true;
    } else {
        if (ext4_read_block(fs, gd_block_bitmap(fs, g), fs->bm_buf) < 0)
            return false;
        fs->bm_group = g;
    }
    fs->bm_next = 0;
    return true;
}

// a free block, near goal when it can be so files stay in one extent. the caller runs
// ext4_alloc_flush once its done
uint64_t ext4_alloc_block(ext4_fs_t *fs, uint64_t goal, bool zero) {
    if (fs->ro)
        return 0;

    uint32_t start    = fs->alloc_group;
    uint32_t goal_bit = UINT32_MAX;
    if (goal >= fs->first_data_block && goal < fs->blocks_count) {
        start    = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
        goal_bit = (uint32_t)((goal - fs->first_data_block) % fs->blocks_per_group);
    }

    for (uint32_t n = 0; n < fs->num_groups; n++) {
        uint32_t g = (start + n) % fs->num_groups;
        if (!ext4_gd_free_blocks(fs, g))
            continue;
        if (!bm_load(fs, g))
            return 0;

        uint32_t count = blocks_in_group(fs, g);
        uint32_t from  = (n == 0 && goal_bit != UINT32_MAX) ? goal_bit : fs->bm_next;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t bit = (from + i) % count;
            if (bit_test(fs->bm_buf, bit))
                continue;

            bit_set(fs->bm_buf, bit);
            fs->bm_next     = bit + 1;
            fs->bm_dirty    = true;
            fs->alloc_group = g;
            gd_free_blocks_add(fs, g, -1);
            sb_free_blocks_add(fs, -1);

            uint64_t blk = group_first_block(fs, g) + bit;
            if (zero) {
                uint8_t *z = kmalloc(fs->block_size);
                if (z) {
                    memset(z, 0, fs->block_size);
                    ext4_write_block(fs, blk, z);
                    kfree(z);
                }
            }
            return blk;
        }
    }
    return 0;
}

void ext4_free_blocks(ext4_fs_t *fs, uint64_t blk, uint64_t count) {
    for (uint64_t b = blk; b < blk + count; b++) {
        if (b < fs->first_data_block || b >= fs->blocks_count) {
            serial_printf(LOG_WARN "ext4: freeing block %lu thats not on the disk\n", b);
            return;
        }
        uint32_t g   = (uint32_t)((b - fs->first_data_block) / fs->blocks_per_group);
        uint32_t bit = (uint32_t)((b - fs->first_data_block) % fs->blocks_per_group);
        if (!bm_load(fs, g))
            return;
        if (!bit_test(fs->bm_buf, bit))
            continue;

        bit_clr(fs->bm_buf, bit);
        fs->bm_dirty = true;
        gd_free_blocks_add(fs, g, 1);
        sb_free_blocks_add(fs, 1);
    }
}

// inodes
uint32_t ext4_alloc_inode(ext4_fs_t *fs, uint32_t near_ino, bool dir) {
    if (fs->ro)
        return 0;

    uint8_t *bm = kmalloc(fs->block_size);
    if (!bm)
        return 0;

    uint32_t first_ino = fs->sb.s_rev_level >= 1 ? fs->sb.s_first_ino : 11;
    uint32_t start     = near_ino ? (near_ino - 1) / fs->inodes_per_group : 0;

    for (uint32_t n = 0; n < fs->num_groups; n++) {
        uint32_t   g  = (start + n) % fs->num_groups;
        ext4_gd_t *gd = ext4_gd(fs, g);
        if (!gd_free_inodes(fs, g))
            continue;

        bool uninit = uninit_flags(fs) && (gd->bg_flags & EXT4_BG_INODE_UNINIT);
        if (uninit) {
            memset(bm, 0, fs->block_size);
            for (uint32_t i = fs->inodes_per_group; i < fs->block_size * 8; i++)
                bit_set(bm, i);
        } else if (ext4_read_block(fs, gd_inode_bitmap(fs, g), bm) < 0) {
            continue;
        }

        for (uint32_t bit = 0; bit < fs->inodes_per_group; bit++) {
            uint32_t ino = g * fs->inodes_per_group + bit + 1;
            if (ino < first_ino || ino > fs->total_inodes || bit_test(bm, bit))
                continue;

            bit_set(bm, bit);
            if (uninit)
                gd->bg_flags &= ~EXT4_BG_INODE_UNINIT;
            ext4_bitmap_csum_set(fs, g, bm, true);
            ext4_write_block(fs, gd_inode_bitmap(fs, g), bm);
            kfree(bm);

            gd_free_inodes_add(fs, g, -1);
            if (dir)
                gd_used_dirs_add(fs, g, 1);
            // e2fsck and linux skip the table past itable_unused, it has to cover us
            if (uninit_flags(fs) && bit >= fs->inodes_per_group - gd_itable_unused(fs, g))
                gd_itable_unused_set(fs, g, fs->inodes_per_group - bit - 1);
            gd_write(fs, g);

            fs->sb.s_free_inodes_count--;
            fs->sb_dirty = true;
            return ino;
        }
    }
    kfree(bm);
    return 0;
}

void ext4_free_inode(ext4_fs_t *fs, uint32_t ino, bool dir) {
    if (ino < 1 || ino > fs->total_inodes)
        return;
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;

    uint8_t *bm = kmalloc(fs->block_size);
    if (!bm)
        return;
    if (ext4_read_block(fs, gd_inode_bitmap(fs, g), bm) < 0 || !bit_test(bm, bit)) {
        kfree(bm);
        return;
    }
    bit_clr(bm, bit);
    ext4_bitmap_csum_set(fs, g, bm, true);
    ext4_write_block(fs, gd_inode_bitmap(fs, g), bm);
    kfree(bm);

    gd_free_inodes_add(fs, g, 1);
    if (dir)
        gd_used_dirs_add(fs, g, -1);
    gd_write(fs, g);

    fs->sb.s_free_inodes_count++;
    fs->sb_dirty = true;
}

static uint64_t inode_offset(ext4_fs_t *fs, uint32_t ino) {
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;
    return ext4_gd_inode_table(fs, g) * fs->block_size + (uint64_t)idx * fs->inode_size;
}

int ext4_read_inode(ext4_fs_t *fs, uint32_t ino, ext4_disk_inode_t *out) {
    if (ino < 1 || ino > fs->total_inodes)
        return -EIO;
    memset(out, 0, sizeof(*out));
    size_t n = fs->inode_size < sizeof(*out) ? fs->inode_size : sizeof(*out);
    return ext4_read_bytes(fs, inode_offset(fs, ino), out, n);
}

// only the part of the record i_extra_isize says is inode goes over the old one, the rest is
// in-inode xattrs. fresh starts from zeros, the table might never have been cleared
int ext4_write_inode(ext4_fs_t *fs, uint32_t ino, const ext4_disk_inode_t *in, bool fresh) {
    if (fs->ro)
        return -EROFS;
    if (ino < 1 || ino > fs->total_inodes)
        return -EIO;

    uint8_t *raw = kmalloc(fs->inode_size);
    if (!raw)
        return -ENOMEM;

    uint64_t off = inode_offset(fs, ino);
    if (fresh)
        memset(raw, 0, fs->inode_size);
    else if (ext4_read_bytes(fs, off, raw, fs->inode_size) < 0) {
        kfree(raw);
        return -EIO;
    }

    size_t n = EXT4_GOOD_OLD_INODE;
    if (fs->inode_size > EXT4_GOOD_OLD_INODE) {
        n += in->i_extra_isize;
        if (n > sizeof(*in))     n = sizeof(*in);
        if (n > fs->inode_size)  n = fs->inode_size;
    }
    memcpy(raw, in, n);

    ext4_inode_csum_set(fs, ino, raw);
    int r = ext4_write_bytes(fs, off, raw, fs->inode_size);
    kfree(raw);
    return r;
}
