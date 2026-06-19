#include <fs/exfat/exfat.h>
#include <fs/vfs.h>

#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>
#include <stddef.h>

#include "exfat_priv.h"

static const INodeOps_t exfat_dir_ops;
static const INodeOps_t exfat_file_ops;

static int exfat_abs_read(exfat_fs_t *fs, uint64_t abs_off, void *buf, size_t count) {
    long r = inode_read(fs->dev, buf, count, abs_off);
    return (r < 0) ? -1 : 0;
}

static int exfat_abs_write(exfat_fs_t *fs, uint64_t abs_off, const void *buf, size_t count) {
    long r = inode_write(fs->dev, buf, count, abs_off);
    return (r < 0) ? -1 : 0;
}

static int exfat_read_bytes(exfat_fs_t *fs, uint32_t lba, size_t off, void *buf, size_t count) {
    uint64_t abs_off = (uint64_t)lba * fs->bytes_per_sector + off;
    return exfat_abs_read(fs, abs_off, buf, count);
}

static int exfat_write_bytes(exfat_fs_t *fs, uint32_t lba, size_t off, const void *buf, size_t count) {
    uint64_t abs_off = (uint64_t)lba * fs->bytes_per_sector + off;
    return exfat_abs_write(fs, abs_off, buf, count);
}

static uint32_t exfat_cluster_to_lba(exfat_fs_t *fs, uint32_t cluster) {
    return fs->cluster_heap_lba + (cluster - EXFAT_FIRST_DATA_CLUSTER) * fs->sectors_per_cluster;
}

// FAT CHAIN

static uint32_t exfat_read_fat(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_off = cluster * 4;
    uint32_t lba     = fs->fat_start_lba + fat_off / fs->bytes_per_sector;
    uint32_t in_off  = fat_off % fs->bytes_per_sector;

    uint32_t val = 0;
    exfat_read_bytes(fs, lba, in_off, &val, sizeof(val));
    return val;
}

static int exfat_write_fat(exfat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_off = cluster * 4;
    uint32_t lba     = fs->fat_start_lba + fat_off / fs->bytes_per_sector;
    uint32_t in_off  = fat_off % fs->bytes_per_sector;
    return exfat_write_bytes(fs, lba, in_off, &value, sizeof(value));
}

// assumed contiguous on disk
static bool exfat_bitmap_get(exfat_fs_t *fs, uint32_t cluster) {
    uint32_t idx        = cluster - EXFAT_FIRST_DATA_CLUSTER;
    uint64_t byte_off    = (uint64_t)exfat_cluster_to_lba(fs, fs->bitmap_cluster) * fs->bytes_per_sector
                           + idx / 8;
    uint8_t b = 0;
    exfat_abs_read(fs, byte_off, &b, 1);
    return (b >> (idx % 8)) & 1;
}

static void exfat_bitmap_set(exfat_fs_t *fs, uint32_t cluster, bool used) {
    uint32_t idx     = cluster - EXFAT_FIRST_DATA_CLUSTER;
    uint64_t byte_off = (uint64_t)exfat_cluster_to_lba(fs, fs->bitmap_cluster) * fs->bytes_per_sector
                        + idx / 8;
    uint8_t b = 0;
    exfat_abs_read(fs, byte_off, &b, 1);
    if (used) b |= (uint8_t)(1u << (idx % 8));
    else      b &= (uint8_t)~(1u << (idx % 8));
    exfat_abs_write(fs, byte_off, &b, 1);
}

static uint32_t exfat_alloc_cluster(exfat_fs_t *fs, uint32_t prev) {
    for (uint32_t c = EXFAT_FIRST_DATA_CLUSTER; c < fs->cluster_count + EXFAT_FIRST_DATA_CLUSTER; c++) {
        if (exfat_bitmap_get(fs, c)) continue;

        exfat_bitmap_set(fs, c, true);
        exfat_write_fat(fs, c, EXFAT_CLUSTER_EOF);
        if (prev != 0)
            exfat_write_fat(fs, prev, c);

        uint8_t zero[512];
        memset(zero, 0, sizeof(zero));
        uint32_t lba = exfat_cluster_to_lba(fs, c);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
            exfat_write_bytes(fs, lba + s, 0, zero, fs->bytes_per_sector);

        return c;
    }
    return 0;
}

// free cluster run
static void exfat_free_extent(exfat_fs_t *fs, uint32_t start, bool no_fat_chain, uint64_t size_bytes) {
    if (start < EXFAT_FIRST_DATA_CLUSTER) return;

    if (no_fat_chain) {
        uint32_t count = (uint32_t)((size_bytes + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);
        for (uint32_t i = 0; i < count; i++)
            exfat_bitmap_set(fs, start + i, false);
        return;
    }

    uint32_t cur = start;
    while (cur >= EXFAT_FIRST_DATA_CLUSTER && cur < EXFAT_CLUSTER_BAD) {
        uint32_t next = exfat_read_fat(fs, cur);
        exfat_write_fat(fs, cur, EXFAT_CLUSTER_FREE);
        exfat_bitmap_set(fs, cur, false);
        cur = next;
    }
}

static uint32_t exfat_cluster_at(exfat_fs_t *fs, uint32_t first_cluster, bool no_fat_chain, uint64_t byte_offset) {
    uint32_t idx = (uint32_t)(byte_offset / fs->bytes_per_cluster);
    if (no_fat_chain)
        return first_cluster + idx;

    uint32_t c = first_cluster;
    for (uint32_t i = 0; i < idx; i++) {
        c = exfat_read_fat(fs, c);
        if (c < EXFAT_FIRST_DATA_CLUSTER || c >= EXFAT_CLUSTER_BAD) return 0;
    }
    return c;
}

// content integrity name hashing etc
static uint16_t exfat_checksum16(const uint8_t *buf, size_t len, uint16_t cksum, bool skip_bytes_2_3) {
    for (size_t i = 0; i < len; i++) {
        if (skip_bytes_2_3 && (i == 2 || i == 3)) continue;
        cksum = (uint16_t)(((cksum << 15) | (cksum >> 1)) & 0xFFFF);
        cksum = (uint16_t)(cksum + buf[i]);
    }
    return cksum;
}

static uint16_t exfat_upcase_char(exfat_fs_t *fs, uint16_t c) {
    if (fs->upcase_table && c < fs->upcase_count)
        return fs->upcase_table[c];
    if (c >= 'a' && c <= 'z')
        return (uint16_t)(c - 32);
    return c;
}

static uint16_t exfat_name_hash(exfat_fs_t *fs, const uint16_t *name, size_t len) {
    uint16_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        uint16_t uc = exfat_upcase_char(fs, name[i]);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) & 0xFFFF);
        hash = (uint16_t)(hash + (uc & 0xFF));
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) & 0xFFFF);
        hash = (uint16_t)(hash + (uc >> 8));
    }
    return hash;
}

static bool exfat_utf8_to_utf16(const char *name, size_t namelen, uint16_t *out, size_t max_chars) {
    if (namelen == 0 || namelen > max_chars) return false;
    for (size_t i = 0; i < namelen; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 0x80) return false;
        out[i] = c;
    }
    return true;
}

static void exfat_utf16_to_utf8(const uint16_t *name, size_t len, char *out, size_t outsz) {
    size_t n = (len < outsz - 1) ? len : outsz - 1;
    size_t i;
    for (i = 0; i < n; i++) {
        uint16_t c = name[i];
        out[i] = (c < 0x80) ? (char)c : '_';
    }
    out[i] = '\0';
}

static bool exfat_name_matches(exfat_fs_t *fs, const uint16_t *entry_name, size_t entry_len,
                                const char *name, size_t namelen) {
    if (entry_len != namelen) return false;
    for (size_t i = 0; i < namelen; i++) {
        uint16_t a = exfat_upcase_char(fs, entry_name[i]);
        uint16_t b = exfat_upcase_char(fs, (uint8_t)name[i]);
        if (a != b) return false;
    }
    return true;
}

typedef struct {
    exfat_fs_t *fs;
    uint32_t    first_cluster;
    bool        no_fat_chain;
    uint32_t    cluster;
    uint32_t    cluster_index;  // for not fat chain
    uint32_t    offset;
} exfat_dir_iter_t;

static void exfat_iter_init(exfat_dir_iter_t *it, exfat_fs_t *fs, uint32_t first_cluster, bool no_fat_chain) {
    it->fs            = fs;
    it->first_cluster = first_cluster;
    it->no_fat_chain  = no_fat_chain;
    it->cluster       = first_cluster;
    it->cluster_index  = 0;
    it->offset        = 0;
}

static bool exfat_iter_next_raw(exfat_dir_iter_t *it, exfat_raw_dentry_t *raw,
                                 uint32_t *out_cluster, uint32_t *out_off) {
    exfat_fs_t *fs = it->fs;
    if (it->cluster < EXFAT_FIRST_DATA_CLUSTER) return false;

    if (it->offset >= fs->bytes_per_cluster) {
        if (it->no_fat_chain) {
            it->cluster_index++;
            it->cluster = it->first_cluster + it->cluster_index;
        } else {
            uint32_t next = exfat_read_fat(fs, it->cluster);
            if (next < EXFAT_FIRST_DATA_CLUSTER || next >= EXFAT_CLUSTER_BAD) return false;
            it->cluster = next;
        }
        it->offset = 0;
    }

    uint32_t lba = exfat_cluster_to_lba(fs, it->cluster);
    if (exfat_read_bytes(fs, lba, it->offset, raw, sizeof(*raw)) < 0) return false;

    *out_cluster = it->cluster;
    *out_off     = it->offset;
    it->offset  += sizeof(*raw);
    return true;
}

#define EXFAT_MAX_NAME_CHARS 255

typedef struct {
    exfat_file_dentry_t   file;
    exfat_stream_dentry_t stream;
    uint16_t              name[EXFAT_MAX_NAME_CHARS];
    size_t                name_len;
    uint32_t              prim_cluster;
    uint32_t              prim_off;
} exfat_entryset_t;

// Returns true with *es filled for the next live file entry-set, false at end-of-directory.
static bool exfat_iter_next_entryset(exfat_dir_iter_t *it, exfat_entryset_t *es) {
    exfat_raw_dentry_t raw;
    uint32_t rc, ro;

    while (exfat_iter_next_raw(it, &raw, &rc, &ro)) {
        if (raw.entry_type == EXFAT_ENTRY_END) return false;
        if (raw.entry_type != EXFAT_ENTRY_FILE) continue;

        memcpy(&es->file, &raw, sizeof(es->file));
        es->prim_cluster = rc;
        es->prim_off     = ro;

        exfat_raw_dentry_t sraw;
        uint32_t sc, so;
        if (!exfat_iter_next_raw(it, &sraw, &sc, &so)) return false;
        if (sraw.entry_type != EXFAT_ENTRY_STREAM) continue; // malformed set, skip it
        memcpy(&es->stream, &sraw, sizeof(es->stream));

        size_t want = es->stream.name_length;
        if (want > EXFAT_MAX_NAME_CHARS) want = EXFAT_MAX_NAME_CHARS;
        size_t got = 0;

        for (uint8_t i = 1; i < es->file.secondary_count; i++) {
            exfat_raw_dentry_t nraw;
            uint32_t nc, no;
            if (!exfat_iter_next_raw(it, &nraw, &nc, &no)) return false;
            if (nraw.entry_type != EXFAT_ENTRY_NAME) continue;

            exfat_name_dentry_t nd;
            memcpy(&nd, &nraw, sizeof(nd));
            for (int j = 0; j < EXFAT_NAME_CHARS_PER_ENTRY && got < want; j++)
                es->name[got++] = nd.name[j];
        }
        es->name_len = got;
        return true;
    }
    return false;
}

// VFS Construction

static INode_t *exfat_make_inode(exfat_fs_t *fs, const exfat_entryset_t *es, INode_t *parent) {
    bool is_dir = (es->file.file_attributes & EXFAT_ATTR_DIRECTORY) != 0;

    exfat_inode_t *fi = kmalloc(sizeof(*fi));
    if (!fi) return NULL;

    fi->fs                = fs;
    fi->first_cluster     = es->stream.first_cluster;
    fi->file_size         = es->stream.data_length;
    fi->valid_data_length  = es->stream.valid_data_length;
    fi->no_fat_chain      = (es->stream.flags & EXFAT_FLAG_NO_FAT_CHAIN) != 0;
    fi->attributes        = es->file.file_attributes;
    fi->dirent_cluster    = es->prim_cluster;
    fi->dirent_offset     = es->prim_off;
    fi->secondary_count   = es->file.secondary_count;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(fi); return NULL; }

    memset(inode, 0, sizeof(*inode));
    inode->type          = is_dir ? INODE_DIRECTORY : INODE_FILE;
    inode->ops           = is_dir ? &exfat_dir_ops : &exfat_file_ops;
    inode->internal_data = fi;
    inode->parent        = parent;
    inode->shared        = 1;

    exfat_utf16_to_utf8(es->name, es->name_len, inode->name, sizeof(inode->name));

    return inode;
}

static void exfat_patch_entryset(exfat_inode_t *fi) {
    exfat_fs_t *fs = fi->fs;

    exfat_stream_dentry_t stream;
    uint32_t lba = exfat_cluster_to_lba(fs, fi->dirent_cluster);
    uint32_t soff = fi->dirent_offset + sizeof(exfat_file_dentry_t);
    exfat_read_bytes(fs, lba, soff, &stream, sizeof(stream));

    stream.flags             = EXFAT_FLAG_ALLOC_POSSIBLE | (fi->no_fat_chain ? EXFAT_FLAG_NO_FAT_CHAIN : 0);
    stream.valid_data_length = fi->valid_data_length;
    stream.first_cluster     = fi->first_cluster;
    stream.data_length       = fi->file_size;
    exfat_write_bytes(fs, lba, soff, &stream, sizeof(stream));

    size_t total = (size_t)(fi->secondary_count + 1) * sizeof(exfat_raw_dentry_t);
    if (total > 768) total = 768; // clamp, matches EXFAT_MAX_NAME_CHARS worst case

    uint8_t buf[768];
    exfat_read_bytes(fs, lba, fi->dirent_offset, buf, total);

    uint16_t cksum = exfat_checksum16(buf, total, 0, true);

    exfat_write_bytes(fs, lba, fi->dirent_offset + 2, &cksum, sizeof(cksum));
}

// inode functions

static int exfat_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result) {
    exfat_inode_t *fi = dir->internal_data;
    exfat_fs_t    *fs = fi->fs;

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

    exfat_dir_iter_t it;
    exfat_iter_init(&it, fs, fi->first_cluster, fi->no_fat_chain);

    exfat_entryset_t es;
    while (exfat_iter_next_entryset(&it, &es)) {
        if (exfat_name_matches(fs, es.name, es.name_len, name, namelen)) {
            INode_t *inode = exfat_make_inode(fs, &es, dir);
            if (!inode) return status_print_error(OUT_OF_MEMORY);
            *result = inode;
            return 0;
        }
    }
    return -FILE_NOT_FOUND;
}

static long exfat_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    exfat_inode_t *fi = inode->internal_data;
    exfat_fs_t    *fs = fi->fs;

    if (offset >= fi->file_size) return 0;
    if (count > fi->file_size - offset) count = fi->file_size - offset;
    if (count == 0) return 0;

    uint32_t cluster = exfat_cluster_at(fs, fi->first_cluster, fi->no_fat_chain, offset);
    if (cluster < EXFAT_FIRST_DATA_CLUSTER) return 0;

    size_t read_total  = 0;
    size_t cluster_off = offset % fs->bytes_per_cluster;

    while (read_total < count && cluster >= EXFAT_FIRST_DATA_CLUSTER && cluster < EXFAT_CLUSTER_BAD) {
        uint32_t lba   = exfat_cluster_to_lba(fs, cluster);
        size_t   avail = fs->bytes_per_cluster - cluster_off;
        size_t   chunk = count - read_total;
        if (chunk > avail) chunk = avail;

        if (exfat_read_bytes(fs, lba, cluster_off, (uint8_t *)buf + read_total, chunk) < 0)
            return read_total > 0 ? (long)read_total : -1;

        read_total  += chunk;
        cluster_off  = 0;

        if (read_total < count) {
            if (fi->no_fat_chain) {
                cluster++;
            } else {
                cluster = exfat_read_fat(fs, cluster);
            }
        }
    }

    return (long)read_total;
}

// Convert a contiguous (NoFatChain) extent into a properly linked FAT chain
static void exfat_chain_ify(exfat_inode_t *fi) {
    exfat_fs_t *fs = fi->fs;
    if (!fi->no_fat_chain || fi->first_cluster < EXFAT_FIRST_DATA_CLUSTER) {
        fi->no_fat_chain = false;
        return;
    }

    uint32_t clusters = (uint32_t)((fi->file_size + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);
    if (clusters == 0) clusters = 1;

    for (uint32_t i = 0; i < clusters; i++) {
        uint32_t cur  = fi->first_cluster + i;
        uint32_t next = (i + 1 < clusters) ? cur + 1 : EXFAT_CLUSTER_EOF;
        exfat_write_fat(fs, cur, next);
    }
    fi->no_fat_chain = false;
}

static long exfat_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    exfat_inode_t *fi = inode->internal_data;
    exfat_fs_t    *fs = fi->fs;

    if (count == 0) return 0;

    uint32_t needed_clusters =
        (uint32_t)((offset + count + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);

    if (fi->first_cluster < EXFAT_FIRST_DATA_CLUSTER) {
        uint32_t c = exfat_alloc_cluster(fs, 0);
        if (!c) return status_print_error(OUT_OF_MEMORY);
        fi->first_cluster = c;
        fi->no_fat_chain  = false;
        exfat_patch_entryset(fi);
    }

    uint32_t have_clusters =
        (uint32_t)((fi->file_size + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);
    if (have_clusters == 0) have_clusters = 1;

    if (needed_clusters > have_clusters) {
        if (fi->no_fat_chain)
            exfat_chain_ify(fi);

        uint32_t cur = fi->first_cluster;
        uint32_t prev = cur;
        uint32_t count_existing = 1;
        while (count_existing < have_clusters) {
            prev = cur;
            cur  = exfat_read_fat(fs, cur);
            count_existing++;
        }
        prev = cur;

        while (have_clusters < needed_clusters) {
            uint32_t nc = exfat_alloc_cluster(fs, prev);
            if (!nc) return status_print_error(OUT_OF_MEMORY);
            prev = nc;
            have_clusters++;
        }
    }

    uint32_t cluster = exfat_cluster_at(fs, fi->first_cluster, fi->no_fat_chain, offset);
    if (cluster < EXFAT_FIRST_DATA_CLUSTER) return status_print_error(OUT_OF_BOUNDS);

    size_t written_total = 0;
    size_t cluster_off    = offset % fs->bytes_per_cluster;

    while (written_total < count && cluster >= EXFAT_FIRST_DATA_CLUSTER && cluster < EXFAT_CLUSTER_BAD) {
        uint32_t lba   = exfat_cluster_to_lba(fs, cluster);
        size_t   avail = fs->bytes_per_cluster - cluster_off;
        size_t   chunk = count - written_total;
        if (chunk > avail) chunk = avail;

        if (exfat_write_bytes(fs, lba, cluster_off, (const uint8_t *)buf + written_total, chunk) < 0)
            return written_total > 0 ? (long)written_total : -1;

        written_total += chunk;
        cluster_off    = 0;

        if (written_total < count) {
            if (fi->no_fat_chain) {
                cluster++;
            } else {
                cluster = exfat_read_fat(fs, cluster);
            }
        }
    }

    size_t new_end = offset + written_total;
    if (new_end > fi->file_size) {
        fi->file_size = new_end;
        fi->valid_data_length = new_end;
        exfat_patch_entryset(fi);
    }

    return (long)written_total;
}

static int exfat_truncate(INode_t *inode, size_t new_size) {
    exfat_inode_t *fi = inode->internal_data;
    exfat_fs_t    *fs = fi->fs;

    if (new_size == fi->file_size) return 0;

    if (new_size == 0) {
        exfat_free_extent(fs, fi->first_cluster, fi->no_fat_chain, fi->file_size);
        fi->first_cluster = 0;
        fi->file_size     = 0;
        fi->valid_data_length = 0;
        fi->no_fat_chain  = false;
    } else if (new_size < fi->file_size) {
        if (fi->no_fat_chain)
            exfat_chain_ify(fi);

        uint32_t keep = (uint32_t)((new_size + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);
        uint32_t cur  = fi->first_cluster;
        for (uint32_t i = 1; i < keep && cur >= EXFAT_FIRST_DATA_CLUSTER; i++)
            cur = exfat_read_fat(fs, cur);

        if (cur >= EXFAT_FIRST_DATA_CLUSTER) {
            uint32_t next = exfat_read_fat(fs, cur);
            exfat_write_fat(fs, cur, EXFAT_CLUSTER_EOF);
            exfat_free_extent(fs, next, false, 0);
        }
        fi->file_size = new_size;
        if (fi->valid_data_length > new_size) fi->valid_data_length = new_size;
    } else {
        uint8_t z = 0;
        long r = exfat_write(inode, &z, 1, new_size - 1);
        if (r < 0) return (int)r;
        fi->file_size = new_size;
        return 0; // exfat_write already patched the entry set
    }

    exfat_patch_entryset(fi);
    return 0;
}

static size_t exfat_size(INode_t *inode) {
    exfat_inode_t *fi = inode->internal_data;
    return fi ? (size_t)fi->file_size : 0;
}

static void exfat_drop(INode_t *inode) {
    if (!inode || !inode->internal_data) return;
    kfree(inode->internal_data);
    inode->internal_data = NULL;
}

static int exfat_readdir(INode_t *dir, size_t index, INode_t **result) {
    exfat_inode_t *fi = dir->internal_data;
    exfat_fs_t    *fs = fi->fs;

    exfat_dir_iter_t it;
    exfat_iter_init(&it, fs, fi->first_cluster, fi->no_fat_chain);

    exfat_entryset_t es;
    size_t visible = 0;
    while (exfat_iter_next_entryset(&it, &es)) {
        if (visible == index) {
            INode_t *inode = exfat_make_inode(fs, &es, dir);
            if (!inode) return status_print_error(OUT_OF_MEMORY);
            inode->shared++;
            *result = inode;
            return 0;
        }
        visible++;
    }
    return -FILE_NOT_FOUND;
}

static bool exfat_slot_free(uint8_t entry_type) {
    return (entry_type & EXFAT_ENTRY_INUSE_BIT) == 0;
}

static int exfat_find_free_run(exfat_inode_t *dirfi, int need, uint32_t *out_cluster, uint32_t *out_off) {
    exfat_fs_t *fs = dirfi->fs;
    if ((uint32_t)need * sizeof(exfat_raw_dentry_t) > fs->bytes_per_cluster)
        return status_print_error(NAME_LIMITS);

    if (dirfi->no_fat_chain)
        exfat_chain_ify(dirfi); // directories we grow must be chain-walkable 

    uint32_t cluster = dirfi->first_cluster;
    uint32_t prev     = 0;

    while (cluster >= EXFAT_FIRST_DATA_CLUSTER && cluster < EXFAT_CLUSTER_BAD) {
        uint32_t lba = exfat_cluster_to_lba(fs, cluster);
        int run = 0;

        for (uint32_t off = 0; off < fs->bytes_per_cluster; off += sizeof(exfat_raw_dentry_t)) {
            uint8_t type = 0;
            exfat_read_bytes(fs, lba, off, &type, 1);

            if (exfat_slot_free(type)) {
                run++;
                if (run == need) {
                    *out_cluster = cluster;
                    *out_off     = off - (uint32_t)(need - 1) * sizeof(exfat_raw_dentry_t);
                    return 0;
                }
            } else {
                run = 0;
            }
        }

        prev    = cluster;
        cluster = exfat_read_fat(fs, cluster);
    }

    uint32_t nc = exfat_alloc_cluster(fs, prev);
    if (!nc) return status_print_error(OUT_OF_MEMORY);

    *out_cluster = nc;
    *out_off     = 0;
    return 0;
}

static void exfat_write_entryset(exfat_fs_t *fs, uint32_t cluster, uint32_t off,
                                  const exfat_file_dentry_t *file, const exfat_stream_dentry_t *stream,
                                  const uint16_t *name, size_t namelen) {
    uint32_t lba = exfat_cluster_to_lba(fs, cluster);

    exfat_write_bytes(fs, lba, off, file, sizeof(*file));
    exfat_write_bytes(fs, lba, off + sizeof(*file), stream, sizeof(*stream));

    size_t remaining = namelen;
    size_t consumed  = 0;
    uint32_t slot_off = off + sizeof(*file) + sizeof(*stream);

    while (remaining > 0 || consumed == 0) {
        exfat_name_dentry_t nd;
        memset(&nd, 0, sizeof(nd));
        nd.entry_type = EXFAT_ENTRY_NAME;

        size_t chunk = remaining < EXFAT_NAME_CHARS_PER_ENTRY ? remaining : EXFAT_NAME_CHARS_PER_ENTRY;
        for (size_t i = 0; i < chunk; i++)
            nd.name[i] = name[consumed + i];

        exfat_write_bytes(fs, lba, slot_off, &nd, sizeof(nd));

        consumed  += chunk;
        remaining -= chunk;
        slot_off  += sizeof(nd);

        if (chunk == 0) break;
    }

    size_t total = sizeof(*file) + sizeof(*stream) + ((namelen + EXFAT_NAME_CHARS_PER_ENTRY - 1) / EXFAT_NAME_CHARS_PER_ENTRY) * sizeof(exfat_name_dentry_t);
    uint8_t buf[768];
    if (total > sizeof(buf)) total = sizeof(buf);
    exfat_read_bytes(fs, lba, off, buf, total);
    uint16_t cksum = exfat_checksum16(buf, total, 0, true);
    exfat_write_bytes(fs, lba, off + 2, &cksum, sizeof(cksum));
}

static int exfat_create(INode_t *parent, const char *name, size_t namelen,
                         INode_t **result, inode_type node_type) {
    exfat_inode_t *pfi = parent->internal_data;
    exfat_fs_t    *fs  = pfi->fs;

    uint16_t name16[EXFAT_MAX_NAME_CHARS];
    if (!exfat_utf8_to_utf16(name, namelen, name16, EXFAT_MAX_NAME_CHARS))
        return status_print_error(NAME_LIMITS);

    uint8_t name_slots = (uint8_t)((namelen + EXFAT_NAME_CHARS_PER_ENTRY - 1) / EXFAT_NAME_CHARS_PER_ENTRY);
    uint8_t secondary_count = (uint8_t)(1 + name_slots); // stream + name entries 

    uint32_t slot_cluster = 0, slot_off = 0;
    int r = exfat_find_free_run(pfi, secondary_count + 1, &slot_cluster, &slot_off);
    if (r < 0) return r;

    exfat_file_dentry_t file;
    memset(&file, 0, sizeof(file));
    file.entry_type      = EXFAT_ENTRY_FILE;
    file.secondary_count = secondary_count;
    file.file_attributes = (node_type == INODE_DIRECTORY) ? EXFAT_ATTR_DIRECTORY : EXFAT_ATTR_ARCHIVE;

    exfat_stream_dentry_t stream;
    memset(&stream, 0, sizeof(stream));
    stream.entry_type  = EXFAT_ENTRY_STREAM;
    stream.flags       = EXFAT_FLAG_ALLOC_POSSIBLE;
    stream.name_length = (uint8_t)namelen;
    stream.name_hash   = exfat_name_hash(fs, name16, namelen);

    uint32_t new_cluster = 0;
    if (node_type == INODE_DIRECTORY) {
        new_cluster = exfat_alloc_cluster(fs, 0);
        if (!new_cluster) return status_print_error(OUT_OF_MEMORY);
        stream.first_cluster     = new_cluster;
        stream.data_length       = fs->bytes_per_cluster;
        stream.valid_data_length = fs->bytes_per_cluster;
    }

    exfat_write_entryset(fs, slot_cluster, slot_off, &file, &stream, name16, namelen);

    exfat_inode_t *fi = kmalloc(sizeof(*fi));
    if (!fi) return status_print_error(OUT_OF_MEMORY);
    fi->fs                = fs;
    fi->first_cluster     = new_cluster;
    fi->file_size         = stream.data_length;
    fi->valid_data_length = stream.valid_data_length;
    fi->no_fat_chain      = false;
    fi->attributes        = file.file_attributes;
    fi->dirent_cluster    = slot_cluster;
    fi->dirent_offset     = slot_off;
    fi->secondary_count   = secondary_count;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(fi); return status_print_error(OUT_OF_MEMORY); }
    memset(inode, 0, sizeof(*inode));
    inode->type          = (node_type == INODE_DIRECTORY) ? INODE_DIRECTORY : INODE_FILE;
    inode->ops           = (node_type == INODE_DIRECTORY) ? &exfat_dir_ops : &exfat_file_ops;
    inode->internal_data = fi;
    inode->parent        = parent;
    inode->shared        = 1;

    *result = inode;
    return 0;
}

static int exfat_unlink(INode_t *dir, const char *name, size_t namelen) {
    exfat_inode_t *fi = dir->internal_data;
    exfat_fs_t    *fs = fi->fs;

    exfat_dir_iter_t it;
    exfat_iter_init(&it, fs, fi->first_cluster, fi->no_fat_chain);

    exfat_entryset_t es;
    while (exfat_iter_next_entryset(&it, &es)) {
        if (!exfat_name_matches(fs, es.name, es.name_len, name, namelen)) continue;

        if (es.file.file_attributes & EXFAT_ATTR_DIRECTORY) {
            exfat_dir_iter_t cit;
            exfat_iter_init(&cit, fs, es.stream.first_cluster,
                             (es.stream.flags & EXFAT_FLAG_NO_FAT_CHAIN) != 0);
            exfat_entryset_t ces;
            if (exfat_iter_next_entryset(&cit, &ces))
                return status_print_error(OUT_OF_BOUNDS); // not empty 
        }

        if (es.stream.first_cluster >= EXFAT_FIRST_DATA_CLUSTER)
            exfat_free_extent(fs, es.stream.first_cluster,
                               (es.stream.flags & EXFAT_FLAG_NO_FAT_CHAIN) != 0,
                               es.stream.data_length);

        // Clear InUse bit on every slot in the set
        uint32_t lba = exfat_cluster_to_lba(fs, es.prim_cluster);
        for (uint8_t i = 0; i <= es.file.secondary_count; i++) {
            uint32_t off = es.prim_off + (uint32_t)i * sizeof(exfat_raw_dentry_t);
            uint8_t type = 0;
            exfat_read_bytes(fs, lba, off, &type, 1);
            type &= (uint8_t)~EXFAT_ENTRY_INUSE_BIT;
            exfat_write_bytes(fs, lba, off, &type, 1);
        }

        return 0;
    }
    return -FILE_NOT_FOUND;
}

static int exfat_rename(INode_t *dir, const char *oldname, size_t oldlen,
                         const char *newname, size_t newlen) {
    exfat_inode_t *fi = dir->internal_data;
    exfat_fs_t    *fs = fi->fs;

    uint16_t new16[EXFAT_MAX_NAME_CHARS];
    if (!exfat_utf8_to_utf16(newname, newlen, new16, EXFAT_MAX_NAME_CHARS))
        return status_print_error(NAME_LIMITS);

    uint8_t new_name_slots = (uint8_t)((newlen + EXFAT_NAME_CHARS_PER_ENTRY - 1) / EXFAT_NAME_CHARS_PER_ENTRY);
    uint8_t new_secondary  = (uint8_t)(1 + new_name_slots);

    exfat_dir_iter_t it;
    exfat_iter_init(&it, fs, fi->first_cluster, fi->no_fat_chain);

    exfat_entryset_t es;
    while (exfat_iter_next_entryset(&it, &es)) {
        if (!exfat_name_matches(fs, es.name, es.name_len, oldname, oldlen)) continue;

        if (new_secondary > es.file.secondary_count)
            return status_print_error(NAME_LIMITS); // would need to grow the set 

        uint32_t lba = exfat_cluster_to_lba(fs, es.prim_cluster);

        // mark any now-unused trailing slots free
        for (uint8_t i = new_secondary; i < es.file.secondary_count; i++) {
            uint32_t off = es.prim_off + (uint32_t)(i + 1) * sizeof(exfat_raw_dentry_t);
            uint8_t type = 0;
            exfat_read_bytes(fs, lba, off, &type, 1);
            type &= (uint8_t)~EXFAT_ENTRY_INUSE_BIT;
            exfat_write_bytes(fs, lba, off, &type, 1);
        }

        es.file.secondary_count = new_secondary;
        es.stream.name_length   = (uint8_t)newlen;
        es.stream.name_hash     = exfat_name_hash(fs, new16, newlen);

        exfat_write_bytes(fs, lba, es.prim_off, &es.file, sizeof(es.file));
        exfat_write_bytes(fs, lba, es.prim_off + sizeof(es.file), &es.stream, sizeof(es.stream));

        size_t remaining = newlen, consumed = 0;
        uint32_t slot_off = es.prim_off + sizeof(es.file) + sizeof(es.stream);
        for (uint8_t i = 0; i < new_name_slots; i++) {
            exfat_name_dentry_t nd;
            memset(&nd, 0, sizeof(nd));
            nd.entry_type = EXFAT_ENTRY_NAME;
            size_t chunk = remaining < EXFAT_NAME_CHARS_PER_ENTRY ? remaining : EXFAT_NAME_CHARS_PER_ENTRY;
            for (size_t j = 0; j < chunk; j++)
                nd.name[j] = new16[consumed + j];
            exfat_write_bytes(fs, lba, slot_off, &nd, sizeof(nd));
            consumed  += chunk;
            remaining -= chunk;
            slot_off  += sizeof(nd);
        }

        size_t total = (size_t)(new_secondary + 1) * sizeof(exfat_raw_dentry_t);
        uint8_t buf[768];
        if (total > sizeof(buf)) total = sizeof(buf);
        exfat_read_bytes(fs, lba, es.prim_off, buf, total);
        uint16_t cksum = exfat_checksum16(buf, total, 0, true);
        exfat_write_bytes(fs, lba, es.prim_off + 2, &cksum, sizeof(cksum));

        return 0;
    }
    return -FILE_NOT_FOUND;
}

static const INodeOps_t exfat_dir_ops = {
    .lookup  = exfat_lookup,
    .readdir = exfat_readdir,
    .create  = exfat_create,
    .unlink  = exfat_unlink,
    .rename  = exfat_rename,
    .drop    = exfat_drop,
};

static const INodeOps_t exfat_file_ops = {
    .read     = exfat_read,
    .write    = exfat_write,
    .truncate = exfat_truncate,
    .size     = exfat_size,
    .drop     = exfat_drop,
};

int exfat_mount(INode_t *dev_inode, INode_t **root) {
    if (!dev_inode || !dev_inode->ops || !dev_inode->ops->read) {
        serial_printf(LOG_ERROR "exfat: device inode missing read op\n");
        return -1;
    }

    uint8_t sector[512];
    long r = inode_read(dev_inode, sector, 512, 0);
    if (r < 512) {
        serial_printf(LOG_ERROR "exfat: failed to read boot sector (got %ld bytes)\n", r);
        return -1;
    }

    exfat_boot_sector_t bs;
    memcpy(&bs, sector, sizeof(bs));

    if (memcmp(bs.fs_name, "EXFAT   ", 8) != 0) {
        serial_printf(LOG_ERROR "exfat: bad fs_name signature\n");
        return -1;
    }
    if (bs.boot_signature != EXFAT_BOOT_SIGNATURE) {
        serial_printf(LOG_ERROR "exfat: missing boot signature (got %04x)\n", bs.boot_signature);
        return -1;
    }
    if (bs.bytes_per_sector_shift < 9 || bs.bytes_per_sector_shift > 12) {
        serial_printf(LOG_ERROR "exfat: invalid bytes_per_sector_shift %u\n", bs.bytes_per_sector_shift);
        return -1;
    }
    if (bs.number_of_fats == 0 || bs.number_of_fats > 2) {
        serial_printf(LOG_ERROR "exfat: invalid number_of_fats %u\n", bs.number_of_fats);
        return -1;
    }

    exfat_fs_t *fs = kmalloc(sizeof(*fs));
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));

    fs->dev                  = dev_inode;
    fs->bytes_per_sector     = 1u << bs.bytes_per_sector_shift;
    fs->sectors_per_cluster  = 1u << bs.sectors_per_cluster_shift;
    fs->bytes_per_cluster    = fs->bytes_per_sector * fs->sectors_per_cluster;
    fs->fat_start_lba        = bs.fat_offset;
    fs->fat_length_sectors   = bs.fat_length;
    fs->cluster_heap_lba     = bs.cluster_heap_offset;
    fs->cluster_count        = bs.cluster_count;
    fs->root_cluster         = bs.first_cluster_of_root_dir;

    serial_printf(LOG_INFO "exfat: bps=%u spc=%u clusters=%u heap_lba=%u root=%u\n",
                  fs->bytes_per_sector, fs->sectors_per_cluster, fs->cluster_count,
                  fs->cluster_heap_lba, fs->root_cluster);

    // Walk the root directory once to find the allocation bitmap and
    exfat_dir_iter_t it;
    exfat_iter_init(&it, fs, fs->root_cluster, false);

    exfat_raw_dentry_t raw;
    uint32_t rc, ro;
    bool have_bitmap = false, have_upcase = false;

    while (exfat_iter_next_raw(&it, &raw, &rc, &ro)) {
        if (raw.entry_type == EXFAT_ENTRY_END) break;

        if (raw.entry_type == EXFAT_ENTRY_BITMAP) {
            exfat_bitmap_dentry_t bd;
            memcpy(&bd, &raw, sizeof(bd));
            fs->bitmap_cluster    = bd.first_cluster;
            fs->bitmap_size_bytes = bd.data_length;
            have_bitmap = true;
        } else if (raw.entry_type == EXFAT_ENTRY_UPCASE) {
            exfat_upcase_dentry_t ud;
            memcpy(&ud, &raw, sizeof(ud));

            size_t count = (size_t)(ud.data_length / 2);
            uint16_t *table = kmalloc(count * sizeof(uint16_t));
            if (table) {
                uint64_t abs_off = (uint64_t)exfat_cluster_to_lba(fs, ud.first_cluster) * fs->bytes_per_sector;
                if (exfat_abs_read(fs, abs_off, table, count * sizeof(uint16_t)) == 0) {
                    fs->upcase_table = table;
                    fs->upcase_count = count;
                    have_upcase = true;
                } else {
                    kfree(table);
                }
            }
        }
    }

    if (!have_bitmap) {
        serial_printf(LOG_ERROR "exfat: no allocation bitmap entry found in root dir\n");
        kfree(fs);
        return -1;
    }
    if (!have_upcase) {
        serial_printf(LOG_WARN "exfat: no up-case table found, falling back to ASCII case folding\n");
    }

    exfat_inode_t *root_fi = kmalloc(sizeof(*root_fi));
    if (!root_fi) { kfree(fs); return -1; }
    memset(root_fi, 0, sizeof(*root_fi));
    root_fi->fs            = fs;
    root_fi->first_cluster = fs->root_cluster;
    root_fi->no_fat_chain  = false;

    INode_t *root_inode = kmalloc(sizeof(*root_inode));
    if (!root_inode) { kfree(root_fi); kfree(fs); return -1; }
    memset(root_inode, 0, sizeof(*root_inode));
    root_inode->type          = INODE_DIRECTORY;
    root_inode->ops           = &exfat_dir_ops;
    root_inode->internal_data = root_fi;
    root_inode->parent        = NULL;
    root_inode->shared        = 1;

    serial_printf(LOG_OK "exfat: mounted - %u clusters, %u B/cluster\n",
                  fs->cluster_count, fs->bytes_per_cluster);

    *root = root_inode;
    return 0;
}