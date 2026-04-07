// please appreciate my comment work, i didnt for fat32 because i felt it was easy but
// i personally found ext2 rather complex and long winded so I commented it for my own good
// they somewhat look inconsistant but theyre mainly for me. hope it helps someone else tho

#include <fs/ext2/ext2.h>
#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>
#include <stddef.h>

#include "ext2_priv.h"

static const INodeOps_t ext2_dir_ops;
static const INodeOps_t ext2_file_ops;

static int ext2_read_bytes(ext2_fs_t *fs, size_t byte_off, void *buf, size_t count) {
    long r = inode_read(fs->dev, buf, count, byte_off);
    return (r == (long)count) ? 0 : -1;
}

static int ext2_write_bytes(ext2_fs_t *fs, size_t byte_off, const void *buf, size_t count) {
    long r = inode_write(fs->dev, buf, count, byte_off);
    return (r == (long)count) ? 0 : -1;
}

static int ext2_read_block(ext2_fs_t *fs, uint32_t block, void *buf) {
    return ext2_read_bytes(fs, (size_t)block * fs->block_size, buf, fs->block_size);
}

static int ext2_write_block(ext2_fs_t *fs, uint32_t block, const void *buf) {
    return ext2_write_bytes(fs, (size_t)block * fs->block_size, buf, fs->block_size);
}

// block group helpers
static int ext2_read_bgd(ext2_fs_t *fs, uint32_t group, ext2_bgd_t *out) {
    uint32_t bgdt_block = fs->first_data_block + 1;
    size_t   off = (size_t)bgdt_block * fs->block_size
                 + (size_t)group * sizeof(ext2_bgd_t);
    return ext2_read_bytes(fs, off, out, sizeof(*out));
}

static int ext2_write_bgd(ext2_fs_t *fs, uint32_t group, const ext2_bgd_t *bgd) {
    uint32_t bgdt_block = fs->first_data_block + 1;
    size_t   off = (size_t)bgdt_block * fs->block_size
                 + (size_t)group * sizeof(ext2_bgd_t);
    return ext2_write_bytes(fs, off, bgd, sizeof(*bgd));
}

// inode operation helpers
static int ext2_read_disk_inode(ext2_fs_t *fs, uint32_t ino, ext2_disk_inode_t *out) {
    if (ino < 1) return -1;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t local = (ino - 1) % fs->inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, group, &bgd) < 0) return -1;

    size_t off = (size_t)bgd.bg_inode_table * fs->block_size
               + (size_t)local * fs->inode_size;

    // ext2_disk_inode_t is exactly 128 bytes anyways
    return ext2_read_bytes(fs, off, out, sizeof(*out));
}

static int ext2_write_disk_inode(ext2_fs_t *fs, uint32_t ino, const ext2_disk_inode_t *in) {
    if (ino < 1) return -1;
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t local = (ino - 1) % fs->inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, group, &bgd) < 0) return -1;

    size_t off = (size_t)bgd.bg_inode_table * fs->block_size
               + (size_t)local * fs->inode_size;
    return ext2_write_bytes(fs, off, in, sizeof(*in));
}

// Block mapping (single/double/triple indirect)
static uint32_t ext2_get_data_block(ext2_fs_t *fs, const ext2_disk_inode_t *di,
                                     uint32_t logical_block) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (logical_block < 12) {   // direct
        return di->i_block[logical_block];
    }
    logical_block -= 12;

    if (logical_block < ptrs_per_block) { // singly indirect
        if (!di->i_block[12]) return 0;
        uint32_t *indirect = kmalloc(fs->block_size);
        if (!indirect) return 0;
        ext2_read_block(fs, di->i_block[12], indirect);
        uint32_t blk = indirect[logical_block];
        kfree(indirect, fs->block_size);
        return blk;
    }
    logical_block -= ptrs_per_block;

    if (logical_block < ptrs_per_block * ptrs_per_block) { // doubly indirect
        if (!di->i_block[13]) return 0;
        uint32_t *dind = kmalloc(fs->block_size);
        if (!dind) return 0;
        ext2_read_block(fs, di->i_block[13], dind);

        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;
        uint32_t ind_blk = dind[idx1];
        kfree(dind, fs->block_size);
        if (!ind_blk) return 0;

        uint32_t *ind = kmalloc(fs->block_size);
        if (!ind) return 0;
        ext2_read_block(fs, ind_blk, ind);
        uint32_t blk = ind[idx2];
        kfree(ind, fs->block_size);
        return blk;
    }

    // tripply indirect
    logical_block -= ptrs_per_block * ptrs_per_block;
    if (!di->i_block[14]) return 0;

    uint32_t *tind = kmalloc(fs->block_size);
    if (!tind) return 0;
    ext2_read_block(fs, di->i_block[14], tind);

    uint32_t idx1 = logical_block / (ptrs_per_block * ptrs_per_block);
    uint32_t rem1 = logical_block % (ptrs_per_block * ptrs_per_block);
    uint32_t dind_blk = tind[idx1];
    kfree(tind, fs->block_size);
    if (!dind_blk) return 0;

    uint32_t *dind = kmalloc(fs->block_size);
    if (!dind) return 0;
    ext2_read_block(fs, dind_blk, dind);
    uint32_t idx2   = rem1 / ptrs_per_block;
    uint32_t idx3   = rem1 % ptrs_per_block;
    uint32_t ind_blk = dind[idx2];
    kfree(dind, fs->block_size);
    if (!ind_blk) return 0;

    uint32_t *ind = kmalloc(fs->block_size);
    if (!ind) return 0;
    ext2_read_block(fs, ind_blk, ind);
    uint32_t blk = ind[idx3];
    kfree(ind, fs->block_size);
    return blk;
}

// alloc a new data block on the fs, returns block number or 0 on failure.
static uint32_t ext2_alloc_block(ext2_fs_t *fs) {
    for (uint32_t g = 0; g < fs->num_groups; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, g, &bgd) < 0) continue;
        if (bgd.bg_free_blocks_count == 0) continue;

        uint8_t *bitmap = kmalloc(fs->block_size);
        if (!bitmap) return 0;
        ext2_read_block(fs, bgd.bg_block_bitmap, bitmap);

        uint32_t blocks_in_group = fs->blocks_per_group;
        
        // its possible for the last group to be smaller
        if ((g + 1) * fs->blocks_per_group > fs->sb.s_blocks_count)
            blocks_in_group = fs->sb.s_blocks_count - g * fs->blocks_per_group;

        for (uint32_t bit = 0; bit < blocks_in_group; bit++) {
            if (!(bitmap[bit / 8] & (1u << (bit % 8)))) {
                bitmap[bit / 8] |= (1u << (bit % 8));
                ext2_write_block(fs, bgd.bg_block_bitmap, bitmap);
                kfree(bitmap, fs->block_size);

                bgd.bg_free_blocks_count--;
                ext2_write_bgd(fs, g, &bgd);

                // update sprblk count
                fs->sb.s_free_blocks_count--;
                ext2_write_bytes(fs, EXT2_SUPERBLOCK_OFFSET, &fs->sb,
                                 sizeof(fs->sb));

                uint32_t blk = fs->first_data_block
                             + g * fs->blocks_per_group + bit;

                // Zero the new block 
                uint8_t *zero = kmalloc(fs->block_size);
                if (zero) {
                    memset(zero, 0, fs->block_size);
                    ext2_write_block(fs, blk, zero);
                    kfree(zero, fs->block_size);
                }
                return blk;
            }
        }
        kfree(bitmap, fs->block_size);
    }
    return 0;
}

static void ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
    if (!blk) return;
    uint32_t g   = (blk - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit = (blk - fs->first_data_block) % fs->blocks_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return;

    uint8_t *bitmap = kmalloc(fs->block_size);
    if (!bitmap) return;
    ext2_read_block(fs, bgd.bg_block_bitmap, bitmap);
    bitmap[bit / 8] &= ~(1u << (bit % 8));
    ext2_write_block(fs, bgd.bg_block_bitmap, bitmap);
    kfree(bitmap, fs->block_size);

    bgd.bg_free_blocks_count++;
    ext2_write_bgd(fs, g, &bgd);

    fs->sb.s_free_blocks_count++;
    ext2_write_bytes(fs, EXT2_SUPERBLOCK_OFFSET, &fs->sb, sizeof(fs->sb));
}

// Set the logical block "logical_block" of inode to physical block "phys". Allocates indirect blocks as required. Returns 0 on success
static int ext2_set_data_block(ext2_fs_t *fs, ext2_disk_inode_t *di,
                                uint32_t ino, uint32_t logical_block, uint32_t phys) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (logical_block < 12) {
        di->i_block[logical_block] = phys;
        return ext2_write_disk_inode(fs, ino, di);
    }
    logical_block -= 12;

    if (logical_block < ptrs_per_block) {
        if (!di->i_block[12]) {
            di->i_block[12] = ext2_alloc_block(fs);
            if (!di->i_block[12]) return -1;
            ext2_write_disk_inode(fs, ino, di);
        }
        uint32_t *ind = kmalloc(fs->block_size);
        if (!ind) return -1;
        ext2_read_block(fs, di->i_block[12], ind);
        ind[logical_block] = phys;
        ext2_write_block(fs, di->i_block[12], ind);
        kfree(ind, fs->block_size);
        return 0;
    }
    logical_block -= ptrs_per_block;

    if (logical_block < ptrs_per_block * ptrs_per_block) {
        if (!di->i_block[13]) {
            di->i_block[13] = ext2_alloc_block(fs);
            if (!di->i_block[13]) return -1;
            ext2_write_disk_inode(fs, ino, di);
        }
        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;

        uint32_t *dind = kmalloc(fs->block_size);
        if (!dind) return -1;
        ext2_read_block(fs, di->i_block[13], dind);
        if (!dind[idx1]) {
            dind[idx1] = ext2_alloc_block(fs);
            if (!dind[idx1]) { kfree(dind, fs->block_size); return -1; }
            ext2_write_block(fs, di->i_block[13], dind);
        }
        uint32_t ind_blk = dind[idx1];
        kfree(dind, fs->block_size);

        uint32_t *ind = kmalloc(fs->block_size);
        if (!ind) return -1;
        ext2_read_block(fs, ind_blk, ind);
        ind[idx2] = phys;
        ext2_write_block(fs, ind_blk, ind);
        kfree(ind, fs->block_size);
        return 0;
    }

    serial_printf(LOG_ERROR "ext2: triple-indirect write not supported\n");
    return -1;
}

// Free all data blocks referenced by an inode's block map
static void ext2_free_inode_blocks(ext2_fs_t *fs, ext2_disk_inode_t *di) {
    uint32_t ptrs = fs->block_size / 4;

    // direct
    for (int i = 0; i < 12; i++) {
        if (di->i_block[i]) ext2_free_block(fs, di->i_block[i]);
    }

    // singly indirect
    if (di->i_block[12]) {
        uint32_t *ind = kmalloc(fs->block_size);
        if (ind) {
            ext2_read_block(fs, di->i_block[12], ind);
            for (uint32_t i = 0; i < ptrs; i++)
                if (ind[i]) ext2_free_block(fs, ind[i]);
            kfree(ind, fs->block_size);
        }
        ext2_free_block(fs, di->i_block[12]);
    }

    // doubly indirect
    if (di->i_block[13]) {
        uint32_t *dind = kmalloc(fs->block_size);
        if (dind) {
            ext2_read_block(fs, di->i_block[13], dind);
            for (uint32_t i = 0; i < ptrs; i++) {
                if (!dind[i]) continue;
                uint32_t *ind = kmalloc(fs->block_size);
                if (ind) {
                    ext2_read_block(fs, dind[i], ind);
                    for (uint32_t j = 0; j < ptrs; j++)
                        if (ind[j]) ext2_free_block(fs, ind[j]);
                    kfree(ind, fs->block_size);
                }
                ext2_free_block(fs, dind[i]);
            }
            kfree(dind, fs->block_size);
        }
        ext2_free_block(fs, di->i_block[13]);
    }

    // Triply indirect
    if (di->i_block[14])
        ext2_free_block(fs, di->i_block[14]);
}

// inode allocation and freeing

static uint32_t ext2_alloc_inode(ext2_fs_t *fs, bool is_dir) {
    for (uint32_t g = 0; g < fs->num_groups; g++) {
        ext2_bgd_t bgd;
        if (ext2_read_bgd(fs, g, &bgd) < 0) continue;
        if (bgd.bg_free_inodes_count == 0) continue;

        uint8_t *bitmap = kmalloc(fs->block_size);
        if (!bitmap) return 0;
        ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap);

        for (uint32_t bit = 0; bit < fs->inodes_per_group; bit++) {
            if (!(bitmap[bit / 8] & (1u << (bit % 8)))) {
                bitmap[bit / 8] |= (1u << (bit % 8));
                ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap);
                kfree(bitmap, fs->block_size);

                bgd.bg_free_inodes_count--;
                if (is_dir) bgd.bg_used_dirs_count++;
                ext2_write_bgd(fs, g, &bgd);

                fs->sb.s_free_inodes_count--;
                ext2_write_bytes(fs, EXT2_SUPERBLOCK_OFFSET, &fs->sb,
                                 sizeof(fs->sb));

                return g * fs->inodes_per_group + bit + 1; // +1 based
            }
        }
        kfree(bitmap, fs->block_size);
    }
    return 0;
}

static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, bool is_dir) {
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;

    ext2_bgd_t bgd;
    if (ext2_read_bgd(fs, g, &bgd) < 0) return;

    uint8_t *bitmap = kmalloc(fs->block_size);
    if (!bitmap) return;
    ext2_read_block(fs, bgd.bg_inode_bitmap, bitmap);
    bitmap[bit / 8] &= ~(1u << (bit % 8));
    ext2_write_block(fs, bgd.bg_inode_bitmap, bitmap);
    kfree(bitmap, fs->block_size);

    bgd.bg_free_inodes_count++;
    if (is_dir && bgd.bg_used_dirs_count > 0)
        bgd.bg_used_dirs_count--;
    ext2_write_bgd(fs, g, &bgd);

    fs->sb.s_free_inodes_count++;
    ext2_write_bytes(fs, EXT2_SUPERBLOCK_OFFSET, &fs->sb, sizeof(fs->sb));
}

static INode_t *ext2_make_vfs_inode(ext2_fs_t *fs, uint32_t ino,
                                     const ext2_disk_inode_t *di,
                                     INode_t *parent) {
    ext2_inode_t *ei = kmalloc(sizeof(*ei));
    if (!ei) return NULL;
    ei->fs  = fs;
    ei->ino = ino;
    memcpy(&ei->disk, di, sizeof(*di));

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(ei, sizeof(*ei)); return NULL; }
    memset(inode, 0, sizeof(*inode));

    uint16_t mode_type = di->i_mode & EXT2_S_IFMT;
    if (mode_type == EXT2_S_IFDIR) {
        inode->type = INODE_DIRECTORY;
        inode->ops  = &ext2_dir_ops;
    } else {
        inode->type = INODE_FILE;
        inode->ops  = &ext2_file_ops;
    }

    inode->internal_data = ei;
    inode->parent        = parent;
    inode->shared        = 1;
    return inode;
}

// inode ops

static long ext2_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    ext2_inode_t     *ei = inode->internal_data;
    ext2_fs_t        *fs = ei->fs;
    ext2_disk_inode_t *di = &ei->disk;

    uint64_t file_size = di->i_size;

    if ((di->i_mode & EXT2_S_IFMT) == EXT2_S_IFREG)
        file_size |= ((uint64_t)di->i_dir_acl << 32);

    if (offset >= file_size) return 0;
    if (count > file_size - offset) count = (size_t)(file_size - offset);
    if (count == 0) return 0;

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    size_t total = 0;
    while (total < count) {
        uint32_t logical = (uint32_t)((offset + total) / fs->block_size);
        size_t   boff    = (offset + total) % fs->block_size;
        uint32_t phys    = ext2_get_data_block(fs, di, logical);
        if (!phys) break;

        if (ext2_read_block(fs, phys, block_buf) < 0) break;

        size_t avail = fs->block_size - boff;
        size_t chunk = count - total;
        if (chunk > avail) chunk = avail;
        memcpy((uint8_t *)buf + total, block_buf + boff, chunk);
        total += chunk;
    }

    kfree(block_buf, fs->block_size);
    return (long)total;
}

static long ext2_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    ext2_inode_t     *ei = inode->internal_data;
    ext2_fs_t        *fs = ei->fs;
    ext2_disk_inode_t *di = &ei->disk;

    if (count == 0) return 0;

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    size_t total = 0;
    while (total < count) {
        uint32_t logical = (uint32_t)((offset + total) / fs->block_size);
        size_t   boff    = (offset + total) % fs->block_size;
        uint32_t phys    = ext2_get_data_block(fs, di, logical);

        if (!phys) {
            phys = ext2_alloc_block(fs);
            if (!phys) break;
            if (ext2_set_data_block(fs, di, ei->ino, logical, phys) < 0) {
                ext2_free_block(fs, phys);
                break;
            }
            // reread cause ext2_set_data_block may have updated it
            ext2_read_disk_inode(fs, ei->ino, di);
        }

        size_t avail = fs->block_size - boff;
        size_t chunk = count - total;
        if (chunk > avail) chunk = avail;

        // partial block
        if (boff != 0 || chunk < fs->block_size) {
            if (ext2_read_block(fs, phys, block_buf) < 0) break;
        }
        memcpy(block_buf + boff, (const uint8_t *)buf + total, chunk);
        if (ext2_write_block(fs, phys, block_buf) < 0) break;
        total += chunk;
    }

    kfree(block_buf, fs->block_size);

    // Update inode size if we extended the file 
    size_t new_end = offset + total;
    if (new_end > di->i_size) {
        di->i_size = (uint32_t)new_end;
        // Update upper 32 bits too (rev1 large files) 
        di->i_dir_acl = (uint32_t)(new_end >> 32);
        di->i_blocks  = (uint32_t)(((uint64_t)new_end + 511) / 512);
        memcpy(&ei->disk, di, sizeof(*di));
        ext2_write_disk_inode(fs, ei->ino, di);
    }

    return (long)total;
}

// more inode impls

static size_t ext2_size(INode_t *inode) {
    ext2_inode_t *ei = inode->internal_data;
    if (!ei) return 0;
    return (size_t)ei->disk.i_size;
}

static int ext2_truncate(INode_t *inode, size_t new_size) {
    ext2_inode_t     *ei = inode->internal_data;
    ext2_fs_t        *fs = ei->fs;
    ext2_disk_inode_t *di = &ei->disk;

    if (new_size == di->i_size) return 0;

    if (new_size == 0) {
        ext2_free_inode_blocks(fs, di);
        memset(di->i_block, 0, sizeof(di->i_block));
        di->i_size   = 0;
        di->i_blocks = 0;
    } else if (new_size < di->i_size) {
        uint32_t keep_blocks = (uint32_t)((new_size + fs->block_size - 1) / fs->block_size);
        uint32_t have_blocks = (uint32_t)((di->i_size + fs->block_size - 1) / fs->block_size);
        for (uint32_t lb = keep_blocks; lb < have_blocks; lb++) {
            uint32_t phys = ext2_get_data_block(fs, di, lb);
            if (phys) ext2_free_block(fs, phys);
            // Clear the map entry
            if (lb < 12) di->i_block[lb] = 0;
        }
        di->i_size   = (uint32_t)new_size;
        di->i_blocks = (uint32_t)((new_size + 511) / 512);
    } else {
        // write 0 byte at the end
        uint8_t z = 0;
        ext2_write(inode, &z, 1, new_size - 1);
        di->i_size = (uint32_t)new_size;
    }

    memcpy(&ei->disk, di, sizeof(*di));
    return ext2_write_disk_inode(fs, ei->ino, di);
}

static void ext2_drop(INode_t *inode) {
    if (!inode || !inode->internal_data) return;
    kfree(inode->internal_data, sizeof(ext2_inode_t));
    inode->internal_data = NULL;
}

typedef struct {
    uint32_t block_idx;   // logical block index within directory 
    uint32_t byte_off;    // byte offset within that block        
    uint32_t phys_block;  // physical block number                
} ext2_dirent_pos_t;

//Iterate over directory entries
typedef bool (*ext2_dirent_cb)(const ext2_dirent_t *de, const char *name,
                                ext2_dirent_pos_t pos, void *ctx);

static void ext2_dir_iterate(ext2_fs_t *fs, ext2_disk_inode_t *di,
                              ext2_dirent_cb cb, void *ctx) {
    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return;

    uint32_t file_size = di->i_size;
    uint32_t lb        = 0;
    uint32_t processed = 0;

    while (processed < file_size) {
        uint32_t phys = ext2_get_data_block(fs, di, lb);
        if (!phys) break;
        if (ext2_read_block(fs, phys, block_buf) < 0) break;

        uint32_t boff = 0;
        while (boff + sizeof(ext2_dirent_t) <= fs->block_size
               && processed + boff < file_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + boff);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                char *name = (char *)(de + 1);
                ext2_dirent_pos_t pos = { lb, boff, phys };
                if (cb(de, name, pos, ctx)) {
                    kfree(block_buf, fs->block_size);
                    return;
                }
            }
            boff += de->rec_len;
        }
        processed += fs->block_size;
        lb++;
    }
    kfree(block_buf, fs->block_size);
}

typedef struct { const char *name; size_t len; uint32_t found_ino; } lookup_ctx_t;

static bool lookup_cb(const ext2_dirent_t *de, const char *name,
                       ext2_dirent_pos_t pos, void *ctx) {
    (void)pos;
    lookup_ctx_t *lc = ctx;
    if (de->name_len == lc->len && memcmp(name, lc->name, lc->len) == 0) {
        lc->found_ino = de->inode;
        return true;
    }
    return false;
}

static int ext2_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result) {
    ext2_inode_t     *ei = dir->internal_data;
    ext2_fs_t        *fs = ei->fs;

    // . and .. handled by vfs_lookup, but handle defensively 
    if (namelen == 1 && name[0] == '.') {
        dir->shared++;
        *result = dir;
        return 0;
    }
    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        INode_t *p = dir->parent ? dir->parent : dir;
        p->shared++;
        *result = p;
        return 0;
    }

    lookup_ctx_t lc = { name, namelen, 0 };
    ext2_dir_iterate(fs, &ei->disk, lookup_cb, &lc);
    if (!lc.found_ino) return -FILE_NOT_FOUND;

    ext2_disk_inode_t disk;
    if (ext2_read_disk_inode(fs, lc.found_ino, &disk) < 0)
        return -FILE_NOT_FOUND;

    INode_t *child = ext2_make_vfs_inode(fs, lc.found_ino, &disk, dir);
    if (!child) return status_print_error(OUT_OF_MEMORY);

    // Copy the name into the inode so getcwd can walk up 
    size_t copy_len = namelen < sizeof(child->name) - 1
                    ? namelen : sizeof(child->name) - 1;
    memcpy(child->name, name, copy_len);
    child->name[copy_len] = '\0';

    *result = child;
    return 0;
}

typedef struct { size_t target; size_t current; uint32_t found_ino; char name[256]; uint8_t ft; } readdir_ctx_t;

static bool readdir_cb(const ext2_dirent_t *de, const char *name,
                        ext2_dirent_pos_t pos, void *ctx) {
    (void)pos;
    readdir_ctx_t *rc = ctx;
    // Skip . and .. 
    if (de->name_len == 1 && name[0] == '.') return false;
    if (de->name_len == 2 && name[0] == '.' && name[1] == '.') return false;

    if (rc->current == rc->target) {
        rc->found_ino = de->inode;
        rc->ft        = de->file_type;
        size_t nl = de->name_len < 255 ? de->name_len : 255;
        memcpy(rc->name, name, nl);
        rc->name[nl] = '\0';
        return true;
    }
    rc->current++;
    return false;
}

static int ext2_readdir(INode_t *dir, size_t index, INode_t **result) {
    ext2_inode_t *ei = dir->internal_data;
    ext2_fs_t    *fs = ei->fs;

    readdir_ctx_t rc = { index, 0, 0, {0}, 0 };
    ext2_dir_iterate(fs, &ei->disk, readdir_cb, &rc);
    if (!rc.found_ino) return -FILE_NOT_FOUND;

    ext2_disk_inode_t disk;
    if (ext2_read_disk_inode(fs, rc.found_ino, &disk) < 0)
        return -FILE_NOT_FOUND;

    INode_t *child = ext2_make_vfs_inode(fs, rc.found_ino, &disk, dir);
    if (!child) return status_print_error(OUT_OF_MEMORY);

    size_t nl = strlen(rc.name);
    size_t copy_len = nl < sizeof(child->name) - 1 ? nl : sizeof(child->name) - 1;
    memcpy(child->name, rc.name, copy_len);
    child->name[copy_len] = '\0';

    child->shared++;
    *result = child;
    return 0;
}

// Append a directory entry to a directory inode
static int ext2_dir_add_entry(ext2_fs_t *fs, ext2_disk_inode_t *parent_di,
                               uint32_t parent_ino,
                               uint32_t child_ino, const char *name,
                               uint8_t namelen, uint8_t file_type) {
    uint16_t needed = (uint16_t)(sizeof(ext2_dirent_t) + namelen);
    // Round to 4-byte boundary 
    needed = (uint16_t)((needed + 3) & ~3u);

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    // Search existing blocks for slack space in the last entry of each block 
    uint32_t file_size = parent_di->i_size;
    uint32_t lb        = 0;
    bool     inserted  = false;

    while (!inserted) {
        uint32_t phys = ext2_get_data_block(fs, parent_di, lb);
        bool     new_block = (phys == 0);

        if (new_block) {
            phys = ext2_alloc_block(fs);
            if (!phys) { kfree(block_buf, fs->block_size); return -1; }
            if (ext2_set_data_block(fs, parent_di, parent_ino, lb, phys) < 0) {
                ext2_free_block(fs, phys);
                kfree(block_buf, fs->block_size);
                return -1;
            }
            ext2_read_disk_inode(fs, parent_ino, parent_di);
            memset(block_buf, 0, fs->block_size);
        } else {
            if (ext2_read_block(fs, phys, block_buf) < 0) {
                kfree(block_buf, fs->block_size);
                return -1;
            }
        }

        // Walk entries
        uint32_t boff = 0;
        while (boff + sizeof(ext2_dirent_t) <= fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + boff);

            if (new_block || de->rec_len == 0) {
                // Empty block
                de->inode     = child_ino;
                de->rec_len   = (uint16_t)(fs->block_size - boff);
                de->name_len  = namelen;
                de->file_type = file_type;
                memcpy((char *)(de + 1), name, namelen);
                ext2_write_block(fs, phys, block_buf);

                // Extend directory size if needed 
                uint32_t new_size = (lb + 1) * fs->block_size;
                if (new_size > file_size) {
                    parent_di->i_size   = new_size;
                    parent_di->i_blocks = (uint32_t)((new_size + 511) / 512);
                    ext2_write_disk_inode(fs, parent_ino, parent_di);
                }
                inserted = true;
                break;
            }

            // Actual size of this entry (based on name_len, rounded to 4) 
            uint16_t actual = (uint16_t)((sizeof(ext2_dirent_t) + de->name_len + 3) & ~3u);
            uint16_t slack  = de->rec_len - actual;

            if (slack >= needed) {
                // Shrink existing entry and insert after it 
                de->rec_len = actual;

                ext2_dirent_t *ne = (ext2_dirent_t *)(block_buf + boff + actual);
                ne->inode     = child_ino;
                ne->rec_len   = slack;
                ne->name_len  = namelen;
                ne->file_type = file_type;
                memcpy((char *)(ne + 1), name, namelen);
                ext2_write_block(fs, phys, block_buf);
                inserted = true;
                break;
            }
            boff += de->rec_len;
        }
        lb++;
    }

    kfree(block_buf, fs->block_size);
    return inserted ? 0 : -1;
}

static int ext2_create(INode_t *parent, const char *name, size_t namelen,
                        INode_t **result, inode_type node_type) {
    if (namelen > 255) return status_print_error(NAME_LIMITS);

    ext2_inode_t     *pei = parent->internal_data;
    ext2_fs_t        *fs  = pei->fs;

    bool is_dir = (node_type == INODE_DIRECTORY);
    uint32_t new_ino = ext2_alloc_inode(fs, is_dir);
    if (!new_ino) return status_print_error(OUT_OF_MEMORY);

    // Initialise the on-disk inode 
    ext2_disk_inode_t new_di;
    memset(&new_di, 0, sizeof(new_di));
    new_di.i_mode        = is_dir ? (EXT2_S_IFDIR | 0755) : (EXT2_S_IFREG | 0644);
    new_di.i_links_count = is_dir ? 2 : 1; // dir: self + parent's entry 
    if (ext2_write_disk_inode(fs, new_ino, &new_di) < 0) {
        ext2_free_inode(fs, new_ino, is_dir);
        return status_print_error(OUT_OF_MEMORY);
    }

    // Add the entry in the parent directory 
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (ext2_dir_add_entry(fs, &pei->disk, pei->ino,
                           new_ino, name, (uint8_t)namelen, ft) < 0) {
        ext2_free_inode(fs, new_ino, is_dir);
        return -1;
    }

    // For directories: add . and .. entries and increment parent link count 
    if (is_dir) {
        // Read the freshly-created inode back to use it as parent for entries 
        ext2_disk_inode_t child_di;
        ext2_read_disk_inode(fs, new_ino, &child_di);
        ext2_dir_add_entry(fs, &child_di, new_ino, new_ino,  ".",  1, EXT2_FT_DIR);
        ext2_dir_add_entry(fs, &child_di, new_ino, pei->ino, "..", 2, EXT2_FT_DIR);

        pei->disk.i_links_count++;
        ext2_write_disk_inode(fs, pei->ino, &pei->disk);
    }

    ext2_disk_inode_t final_di;
    if (ext2_read_disk_inode(fs, new_ino, &final_di) < 0) {
        ext2_free_inode(fs, new_ino, is_dir);
        return -1;
    }

    INode_t *child_inode = ext2_make_vfs_inode(fs, new_ino, &final_di, parent);
    if (!child_inode) {
        ext2_free_inode(fs, new_ino, is_dir);
        return status_print_error(OUT_OF_MEMORY);
    }

    size_t copy_len = namelen < sizeof(child_inode->name) - 1
                    ? namelen : sizeof(child_inode->name) - 1;
    memcpy(child_inode->name, name, copy_len);
    child_inode->name[copy_len] = '\0';

    *result = child_inode;
    return 0;
} 

typedef struct {
    const char *name;
    size_t      len;
    // out 
    uint32_t    found_ino;
    uint32_t    phys_block;
    uint32_t    entry_off;
    uint32_t    prev_off;
    uint16_t    prev_rec_len;
} unlink_ctx_t;

static bool unlink_cb(const ext2_dirent_t *de, const char *name,
                       ext2_dirent_pos_t pos, void *ctx) {
    unlink_ctx_t *uc = ctx;
    if (de->name_len == uc->len && memcmp(name, uc->name, uc->len) == 0) {
        uc->found_ino  = de->inode;
        uc->phys_block = pos.phys_block;
        uc->entry_off  = pos.byte_off;
        return true;
    }
    return false;
}

static int ext2_unlink(INode_t *dir, const char *name, size_t namelen) {
    ext2_inode_t     *ei = dir->internal_data;
    ext2_fs_t        *fs = ei->fs;

    unlink_ctx_t uc = { name, namelen, 0, 0, 0, UINT32_MAX, 0 };
    ext2_dir_iterate(fs, &ei->disk, unlink_cb, &uc);
    if (!uc.found_ino) return -FILE_NOT_FOUND;

    // Check whether it's a non-empty directory 
    ext2_disk_inode_t target_di;
    if (ext2_read_disk_inode(fs, uc.found_ino, &target_di) < 0)
        return -FILE_NOT_FOUND;

    bool target_is_dir = ((target_di.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
    if (target_is_dir) {
        readdir_ctx_t rc = { 0, 0, 0, {0}, 0 };
        ext2_dir_iterate(fs, &target_di, readdir_cb, &rc);
        if (rc.found_ino != 0) return status_print_error(OUT_OF_BOUNDS); // not empty ?
    }

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext2_read_block(fs, uc.phys_block, block_buf) < 0) {
        kfree(block_buf, fs->block_size);
        return -1;
    }

    // Walk the block to find the entry before ours so we can coalesce 
    uint32_t boff = 0;
    uint32_t prev = UINT32_MAX;
    while (boff < fs->block_size) {
        ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + boff);
        if (de->rec_len == 0) break;
        if (boff == uc.entry_off) {
            if (prev != UINT32_MAX) {
                ext2_dirent_t *pde = (ext2_dirent_t *)(block_buf + prev);
                pde->rec_len += de->rec_len;
            } else {
                // First entry so we just zero it out 
                de->inode = 0;
            }
            break;
        }
        prev = boff;
        boff += de->rec_len;
    }
    ext2_write_block(fs, uc.phys_block, block_buf);
    kfree(block_buf, fs->block_size);

    target_di.i_links_count--;
    if (target_is_dir) {
        ei->disk.i_links_count--;
        ext2_write_disk_inode(fs, ei->ino, &ei->disk);
    }
    if (target_di.i_links_count == 0) {
        ext2_free_inode_blocks(fs, &target_di);
        memset(target_di.i_block, 0, sizeof(target_di.i_block));
        target_di.i_size   = 0;
        target_di.i_dtime  = 1; // non-zero marks deleted 
        ext2_write_disk_inode(fs, uc.found_ino, &target_di);
        ext2_free_inode(fs, uc.found_ino, target_is_dir);
    } else {
        ext2_write_disk_inode(fs, uc.found_ino, &target_di);
    }

    return 0;
}

static int ext2_rename(INode_t *dir, const char *oldname, size_t oldlen,
                        const char *newname, size_t newlen) {
    ext2_inode_t     *ei = dir->internal_data;
    ext2_fs_t        *fs = ei->fs;

    if (newlen > 255) return status_print_error(NAME_LIMITS);

    // Find the old entry 
    unlink_ctx_t uc = { oldname, oldlen, 0, 0, 0, UINT32_MAX, 0 };
    ext2_dir_iterate(fs, &ei->disk, unlink_cb, &uc);
    if (!uc.found_ino) return -FILE_NOT_FOUND;

    uint8_t *block_buf = kmalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext2_read_block(fs, uc.phys_block, block_buf) < 0) {
        kfree(block_buf, fs->block_size);
        return -1;
    }

    ext2_dirent_t *de = (ext2_dirent_t *)(block_buf + uc.entry_off);
    uint16_t new_needed = (uint16_t)((sizeof(ext2_dirent_t) + newlen + 3) & ~3u);

    if (new_needed <= de->rec_len) {
        de->name_len = (uint8_t)newlen;
        memcpy((char *)(de + 1), newname, newlen);
        // Zero out any leftover bytes in the name field
        if (newlen < oldlen)
            memset((char *)(de + 1) + newlen, 0, oldlen - newlen);
        ext2_write_block(fs, uc.phys_block, block_buf);
        kfree(block_buf, fs->block_size);
        return 0;
    }

    uint32_t target_ino = de->inode;
    uint8_t  ft         = de->file_type;

    uint32_t boff = 0;
    uint32_t prev = UINT32_MAX;
    while (boff < fs->block_size) {
        ext2_dirent_t *e = (ext2_dirent_t *)(block_buf + boff);
        if (e->rec_len == 0) break;
        if (boff == uc.entry_off) {
            if (prev != UINT32_MAX)
                ((ext2_dirent_t *)(block_buf + prev))->rec_len += e->rec_len;
            else
                e->inode = 0;
            break;
        }
        prev = boff;
        boff += e->rec_len;
    }
    ext2_write_block(fs, uc.phys_block, block_buf);
    kfree(block_buf, fs->block_size);

    ext2_read_disk_inode(fs, ei->ino, &ei->disk);
    return ext2_dir_add_entry(fs, &ei->disk, ei->ino,
                              target_ino, newname, (uint8_t)newlen, ft);
}

static const INodeOps_t ext2_dir_ops = {
    .lookup  = ext2_lookup,
    .readdir = ext2_readdir,
    .create  = ext2_create,
    .unlink  = ext2_unlink,
    .rename  = ext2_rename,
    .drop    = ext2_drop,
};

static const INodeOps_t ext2_file_ops = {
    .read     = ext2_read,
    .write    = ext2_write,
    .truncate = ext2_truncate,
    .size     = ext2_size,
    .drop     = ext2_drop,
};

int ext2_mount(INode_t *dev_inode, INode_t **root) {
    if (!dev_inode || !dev_inode->ops || !dev_inode->ops->read) {
        serial_printf(LOG_ERROR "ext2: null or invalid device inode\n");
        return -1;
    }

    // Read the superblock (byte offset 1024, size 1024) 
    ext2_superblock_t sb;
    long r = inode_read(dev_inode, &sb, sizeof(sb), EXT2_SUPERBLOCK_OFFSET);
    if (r != (long)sizeof(sb)) {
        serial_printf(LOG_ERROR "ext2: failed to read superblock (got %ld)\n", r);
        return -1;
    }

    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        serial_printf(LOG_ERROR "ext2: bad magic 0x%04x (expected 0x%04x)\n",
                      sb.s_magic, EXT2_SUPER_MAGIC);
        return -1;
    }

    // Reject features we haven't implemented 
    if (sb.s_feature_incompat & ~0x2u) {
        // bit 1 = filetype field in dirents anything else, we tell it no
        serial_printf(LOG_ERROR "ext2: unsupported incompat features 0x%x\n",
                      sb.s_feature_incompat);
        return -1;
    }

    ext2_fs_t *fs = kmalloc(sizeof(*fs));
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));

    fs->dev              = dev_inode;
    fs->block_size       = 1024u << sb.s_log_block_size;
    fs->blocks_per_group = sb.s_blocks_per_group;
    fs->inodes_per_group = sb.s_inodes_per_group;
    fs->first_data_block = sb.s_first_data_block;
    fs->total_inodes     = sb.s_inodes_count;
    fs->inode_size       = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128;
    fs->num_groups       = (sb.s_blocks_count + sb.s_blocks_per_group - 1)
                           / sb.s_blocks_per_group;
    memcpy(&fs->sb, &sb, sizeof(sb));

    serial_printf(LOG_INFO "ext2: block_size=%u groups=%u inodes_per_group=%u "
                  "inode_size=%u rev=%u\n",
                  fs->block_size, fs->num_groups,
                  fs->inodes_per_group, fs->inode_size, sb.s_rev_level);

    // Read the root inode
    ext2_disk_inode_t root_di;
    if (ext2_read_disk_inode(fs, EXT2_ROOT_INO, &root_di) < 0) {
        serial_printf(LOG_ERROR "ext2: failed to read root inode\n");
        kfree(fs, sizeof(*fs));
        return -1;
    }

    if ((root_di.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        serial_printf(LOG_ERROR "ext2: inode 2 is not a directory (mode=0x%x)\n",
                      root_di.i_mode);
        kfree(fs, sizeof(*fs));
        return -1;
    }

    INode_t *root_inode = ext2_make_vfs_inode(fs, EXT2_ROOT_INO, &root_di, NULL);
    if (!root_inode) {
        kfree(fs, sizeof(*fs));
        return -1;
    }

    serial_printf(LOG_OK "ext2: mounted - %u blocks, %u inodes, block_size=%u\n",
                  sb.s_blocks_count, sb.s_inodes_count, fs->block_size);

    *root = root_inode;
    return 0;
}