#include "ext4_priv.h"

#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <user/errno.h>

#define EXT_MAX_DEPTH   5
#define EXT_ROOT_MAX    4

#define EH(buf)   ((ext4_extent_header_t *)(buf))
#define EXT(buf)  ((ext4_extent_t *)((uint8_t *)(buf) + sizeof(ext4_extent_header_t)))
#define IDX(buf)  ((ext4_extent_idx_t *)((uint8_t *)(buf) + sizeof(ext4_extent_header_t)))
// ee_block and ei_block both sit right after the header
#define FIRST_KEY(buf) (*(uint32_t *)((uint8_t *)(buf) + sizeof(ext4_extent_header_t)))

void ext4_iblocks_add(ext4_inode_t *ei, int64_t fs_blocks) {
    ext4_disk_inode_t *di  = &ei->disk;
    uint64_t           cur = di->i_blocks_lo | ((uint64_t)di->i_blocks_high << 32);
    if (di->i_flags & EXT4_HUGE_FILE_FL)
        cur += fs_blocks;
    else
        cur += fs_blocks * (ei->fs->block_size / 512);
    di->i_blocks_lo   = (uint32_t)cur;
    di->i_blocks_high = (uint16_t)(cur >> 32);
}

// extent helpers
static inline uint64_t ext_start(const ext4_extent_t *e) {
    return e->ee_start_lo | ((uint64_t)e->ee_start_hi << 32);
}

static inline void ext_start_set(ext4_extent_t *e, uint64_t p) {
    e->ee_start_lo = (uint32_t)p;
    e->ee_start_hi = (uint16_t)(p >> 32);
}

static inline bool ext_unwritten(const ext4_extent_t *e) {
    return e->ee_len > EXT4_EXT_INIT_MAX;
}

static inline uint32_t ext_len(const ext4_extent_t *e) {
    return ext_unwritten(e) ? e->ee_len - EXT4_EXT_INIT_MAX : e->ee_len;
}

static inline void ext_len_set(ext4_extent_t *e, uint32_t len, bool unwritten) {
    e->ee_len = (uint16_t)(unwritten ? len + EXT4_EXT_INIT_MAX : len);
}

static inline uint64_t idx_leaf(const ext4_extent_idx_t *ix) {
    return ix->ei_leaf_lo | ((uint64_t)ix->ei_leaf_hi << 32);
}

static inline uint16_t ext_block_max(ext4_fs_t *fs) {
    return (uint16_t)((fs->block_size - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_t));
}

static bool ext_header_ok(const ext4_extent_header_t *h, int depth, uint16_t max) {
    return h->eh_magic == EXT4_EXT_MAGIC && h->eh_depth == depth &&
           h->eh_max <= max && h->eh_entries <= h->eh_max;
}

static ext4_extent_t ext_make(uint32_t lblk, uint32_t len, uint64_t pblk, bool unwritten) {
    ext4_extent_t e = { .ee_block = lblk };
    ext_len_set(&e, len, unwritten);
    ext_start_set(&e, pblk);
    return e;
}

static int ext_map(ext4_inode_t *ei, uint32_t lblk, uint64_t *pblk, uint32_t *len, bool *unwritten) {
    ext4_fs_t     *fs    = ei->fs;
    const uint8_t *node  = (const uint8_t *)ei->disk.i_block;
    int            depth = EH(node)->eh_depth;
    uint16_t       max   = EXT_ROOT_MAX;

    if (depth > EXT_MAX_DEPTH)
        return -EIO;

    for (int level = depth; level > 0; level--) {
        const ext4_extent_header_t *h = EH(node);
        if (!ext_header_ok(h, level, max) || !h->eh_entries)
            return -EIO;

        const ext4_extent_idx_t *ix = IDX(node);
        int i = 0;
        while (i + 1 < h->eh_entries && ix[i + 1].ei_block <= lblk)
            i++;
        node = ext4_cached_block(fs, idx_leaf(&ix[i]));
        if (!node)
            return -EIO;
        max = ext_block_max(fs);
    }

    const ext4_extent_header_t *h = EH(node);
    if (!ext_header_ok(h, 0, max))
        return -EIO;

    *pblk      = 0;
    *len       = 1;
    *unwritten = false;

    const ext4_extent_t *e = EXT(node);
    for (int i = 0; i < h->eh_entries; i++) {
        uint32_t s = e[i].ee_block;
        uint32_t n = ext_len(&e[i]);
        if (lblk < s) {
            *len = s - lblk;
            return 0;
        }
        if (lblk - s < n) {
            *pblk      = ext_start(&e[i]) + (lblk - s);
            *len       = n - (lblk - s);
            *unwritten = ext_unwritten(&e[i]);
            return 0;
        }
    }
    return 0;
}

// a path from the root in the inode down to the leaf that lblk belongs in
typedef struct {
    uint64_t  block;    // 0 = the root in i_block
    uint8_t  *buf;
    int       slot;     // entry taken at this level, -1 in a leaf = before the first
} ext_path_t;

static void path_free(ext_path_t *p, int last) {
    for (int l = 1; l <= last; l++)
        kfree(p[l].buf);
}

static int path_find(ext4_inode_t *ei, uint32_t lblk, ext_path_t *p, int *depth_out) {
    ext4_fs_t *fs    = ei->fs;
    int        depth = EH(ei->disk.i_block)->eh_depth;

    if (depth > EXT_MAX_DEPTH)
        return -EIO;

    p[0].block = 0;
    p[0].buf   = (uint8_t *)ei->disk.i_block;

    for (int l = 0; ; l++) {
        ext4_extent_header_t *h = EH(p[l].buf);
        if (!ext_header_ok(h, depth - l, l ? ext_block_max(fs) : EXT_ROOT_MAX)) {
            path_free(p, l);
            return -EIO;
        }

        if (l == depth) {
            ext4_extent_t *e = EXT(p[l].buf);
            int i = -1;
            while (i + 1 < h->eh_entries && e[i + 1].ee_block <= lblk)
                i++;
            p[l].slot = i;
            break;
        }

        if (!h->eh_entries) {
            path_free(p, l);
            return -EIO;
        }
        ext4_extent_idx_t *ix = IDX(p[l].buf);
        int i = 0;
        while (i + 1 < h->eh_entries && ix[i + 1].ei_block <= lblk)
            i++;
        p[l].slot = i;

        p[l + 1].block = idx_leaf(&ix[i]);
        p[l + 1].buf   = kmalloc(fs->block_size);
        if (!p[l + 1].buf || ext4_read_block(fs, p[l + 1].block, p[l + 1].buf) < 0) {
            path_free(p, l + 1);
            return -EIO;
        }
    }
    *depth_out = depth;
    return 0;
}

static int node_write(ext4_inode_t *ei, ext_path_t *p) {
    if (!p->block)
        return ext4_write_inode(ei->fs, ei->ino, &ei->disk, false);
    ext4_extent_csum_set(ei, p->buf);
    return ext4_write_block(ei->fs, p->block, p->buf);
}

// the first key at level changed, the index above has to say the same (e2fsck checks)
static void fix_keys(ext4_inode_t *ei, ext_path_t *p, int level) {
    for (; level > 0; level--) {
        ext4_extent_idx_t *ix = IDX(p[level - 1].buf) + p[level - 1].slot;
        if (ix->ei_block == FIRST_KEY(p[level].buf))
            return;
        ix->ei_block = FIRST_KEY(p[level].buf);
        node_write(ei, &p[level - 1]);
        if (p[level - 1].slot)
            return;
    }
}

// the root is full all the way down, move it into a block and point the root at that
static int ext_grow(ext4_inode_t *ei, uint64_t goal) {
    ext4_fs_t            *fs   = ei->fs;
    ext4_extent_header_t *root = EH(ei->disk.i_block);

    if (root->eh_depth >= EXT_MAX_DEPTH)
        return -ENOSPC;

    uint64_t blk = ext4_alloc_block(fs, goal, false);
    if (!blk)
        return -ENOSPC;
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) {
        ext4_free_blocks(fs, blk, 1);
        return -ENOMEM;
    }
    memset(buf, 0, fs->block_size);
    memcpy(buf, root, sizeof(*root) + root->eh_entries * sizeof(ext4_extent_t));
    EH(buf)->eh_max = ext_block_max(fs);
    ext4_extent_csum_set(ei, buf);
    int r = ext4_write_block(fs, blk, buf);
    kfree(buf);
    if (r < 0) {
        ext4_free_blocks(fs, blk, 1);
        return r;
    }

    ext4_extent_idx_t *ix = IDX(root);
    uint32_t first = FIRST_KEY(root);
    memset(ix, 0, sizeof(ei->disk.i_block) - sizeof(*root));
    ix->ei_block   = first;
    ix->ei_leaf_lo = (uint32_t)blk;
    ix->ei_leaf_hi = (uint16_t)(blk >> 32);
    root->eh_depth++;
    root->eh_entries = 1;
    ext4_iblocks_add(ei, 1);
    return ext4_write_inode(fs, ei->ino, &ei->disk, false);
}

// the node at level is full and its parent isnt, half of it goes to a new sibling.
static int ext_split(ext4_inode_t *ei, ext_path_t *p, int level) {
    ext4_fs_t            *fs = ei->fs;
    ext4_extent_header_t *h  = EH(p[level].buf);
    uint16_t              n  = h->eh_entries;

    if (n < 2)
        return -EIO;
    uint16_t m = p[level].slot >= n - 1 ? n - 1 : n / 2;

    uint64_t blk = ext4_alloc_block(fs, p[level].block + 1, false);
    if (!blk)
        return -ENOSPC;
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) {
        ext4_free_blocks(fs, blk, 1);
        return -ENOMEM;
    }
    memset(buf, 0, fs->block_size);

    ext4_extent_header_t *nh = EH(buf);
    nh->eh_magic   = EXT4_EXT_MAGIC;
    nh->eh_entries = n - m;
    nh->eh_max     = ext_block_max(fs);
    nh->eh_depth   = h->eh_depth;

    uint8_t *moved = p[level].buf + sizeof(*h) + (size_t)m * sizeof(ext4_extent_t);
    memcpy(buf + sizeof(*h), moved, (size_t)(n - m) * sizeof(ext4_extent_t));
    memset(moved, 0, (size_t)(n - m) * sizeof(ext4_extent_t));
    h->eh_entries = m;

    ext4_extent_csum_set(ei, buf);
    int r = ext4_write_block(fs, blk, buf);
    uint32_t key = FIRST_KEY(buf);
    kfree(buf);
    if (r < 0 || (r = node_write(ei, &p[level])) < 0)
        return r;

    ext4_extent_header_t *ph = EH(p[level - 1].buf);
    ext4_extent_idx_t    *pix = IDX(p[level - 1].buf);
    int at = p[level - 1].slot + 1;
    memmove(&pix[at + 1], &pix[at], (size_t)(ph->eh_entries - at) * sizeof(*pix));
    memset(&pix[at], 0, sizeof(*pix));
    pix[at].ei_block   = key;
    pix[at].ei_leaf_lo = (uint32_t)blk;
    pix[at].ei_leaf_hi = (uint16_t)(blk >> 32);
    ph->eh_entries++;
    ext4_iblocks_add(ei, 1);
    return node_write(ei, &p[level - 1]);
}

static bool ext_can_merge(const ext4_extent_t *a, const ext4_extent_t *b) {
    if (ext_unwritten(a) != ext_unwritten(b))
        return false;
    uint32_t la  = ext_len(a);
    uint32_t cap = ext_unwritten(a) ? EXT4_EXT_INIT_MAX - 1 : EXT4_EXT_INIT_MAX;
    return a->ee_block + la == b->ee_block && ext_start(a) + la == ext_start(b) && la + ext_len(b) <= cap;
}

// ex has to land in a hole
static int ext_insert(ext4_inode_t *ei, ext4_extent_t ex) {
    for (int tries = 0; tries < 4 * EXT_MAX_DEPTH; tries++) {
        ext_path_t p[EXT_MAX_DEPTH + 1];
        int depth;
        int r = path_find(ei, ex.ee_block, p, &depth);
        if (r < 0)
            return r;

        ext4_extent_header_t *h  = EH(p[depth].buf);
        ext4_extent_t        *e  = EXT(p[depth].buf);
        int                   at = p[depth].slot + 1;

        if (at > 0 && ext_can_merge(&e[at - 1], &ex)) {
            ext_len_set(&e[at - 1], ext_len(&e[at - 1]) + ext_len(&ex), ext_unwritten(&ex));
            r = node_write(ei, &p[depth]);
            path_free(p, depth);
            return r;
        }
        if (at < h->eh_entries && ext_can_merge(&ex, &e[at])) {
            ext_len_set(&e[at], ext_len(&e[at]) + ext_len(&ex), ext_unwritten(&ex));
            e[at].ee_block = ex.ee_block;
            ext_start_set(&e[at], ext_start(&ex));
            r = node_write(ei, &p[depth]);
            if (at == 0)
                fix_keys(ei, p, depth);
            path_free(p, depth);
            return r;
        }
        if (h->eh_entries < h->eh_max) {
            memmove(&e[at + 1], &e[at], (size_t)(h->eh_entries - at) * sizeof(*e));
            e[at] = ex;
            h->eh_entries++;
            r = node_write(ei, &p[depth]);
            if (at == 0)
                fix_keys(ei, p, depth);
            path_free(p, depth);
            return r;
        }

        // no room in the leaf, make some and go again
        int k = depth - 1;
        while (k >= 0 && EH(p[k].buf)->eh_entries >= EH(p[k].buf)->eh_max)
            k--;
        r = k < 0 ? ext_grow(ei, ext_start(&ex)) : ext_split(ei, p, k + 1);
        path_free(p, depth);
        if (r < 0)
            return r;
    }
    serial_printf(LOG_ERROR "ext4: extent insert on inode %u never found room\n", ei->ino);
    return -EIO;
}

// everything at or past from goes, returns how many entries are left in the node
static int ext_trunc_node(ext4_inode_t *ei, uint8_t *buf, int depth, uint32_t from) {
    ext4_fs_t            *fs = ei->fs;
    ext4_extent_header_t *h  = EH(buf);

    if (depth == 0) {
        ext4_extent_t *e = EXT(buf);
        while (h->eh_entries) {
            ext4_extent_t *x = &e[h->eh_entries - 1];
            uint32_t s = x->ee_block;
            uint32_t n = ext_len(x);
            if (s >= from) {
                ext4_free_blocks(fs, ext_start(x), n);
                ext4_iblocks_add(ei, -(int64_t)n);
                memset(x, 0, sizeof(*x));
                h->eh_entries--;
                continue;
            }
            if (s + n > from) {
                uint32_t keep = from - s;
                ext4_free_blocks(fs, ext_start(x) + keep, n - keep);
                ext4_iblocks_add(ei, -(int64_t)(n - keep));
                ext_len_set(x, keep, ext_unwritten(x));
            }
            break;
        }
        return h->eh_entries;
    }

    uint8_t *child = kmalloc(fs->block_size);
    if (!child)
        return h->eh_entries;

    ext4_extent_idx_t *ix = IDX(buf);
    while (h->eh_entries) {
        ext4_extent_idx_t *c   = &ix[h->eh_entries - 1];
        uint32_t           key = c->ei_block;
        uint64_t           cb  = idx_leaf(c);

        if (ext4_read_block(fs, cb, child) < 0 || !ext_header_ok(EH(child), depth - 1, ext_block_max(fs))) {
            serial_printf(LOG_ERROR "ext4: bad extent block %lu in inode %u, leaving it\n", cb, ei->ino);
            break;
        }
        if (ext_trunc_node(ei, child, depth - 1, from) == 0) {
            ext4_free_blocks(fs, cb, 1);
            ext4_iblocks_add(ei, -1);
            memset(c, 0, sizeof(*c));
            h->eh_entries--;
        } else {
            ext4_extent_csum_set(ei, child);
            ext4_write_block(fs, cb, child);
        }
        // children before this one all end before its key
        if (key < from)
            break;
    }
    kfree(child);
    return h->eh_entries;
}

// indirect map
static uint32_t ind_entry(ext4_fs_t *fs, uint32_t blk, uint32_t idx) {
    uint8_t *b = ext4_cached_block(fs, blk);
    return b ? ((uint32_t *)b)[idx] : 0;
}

// which i_block slot lblk hangs off and how many levels down, -1 past the end
static int ind_split(ext4_fs_t *fs, uint32_t lblk, uint64_t *rel) {
    uint64_t p1 = fs->block_size / 4, p2 = p1 * p1, p3 = p2 * p1;
    uint64_t l  = lblk;

    if (l < 12) { *rel = l; return 0; }
    l -= 12;
    if (l < p1) { *rel = l; return 1; }
    l -= p1;
    if (l < p2) { *rel = l; return 2; }
    l -= p2;
    if (l < p3) { *rel = l; return 3; }
    return -1;
}

static uint64_t ind_get(ext4_inode_t *ei, uint32_t lblk) {
    ext4_fs_t *fs   = ei->fs;
    uint32_t   ptrs = fs->block_size / 4;
    uint64_t   rel;
    int        levels = ind_split(fs, lblk, &rel);

    if (levels <= 0)
        return levels == 0 ? ei->disk.i_block[rel] : 0;

    uint32_t blk = ei->disk.i_block[11 + levels];
    for (int lv = levels; lv >= 1 && blk; lv--) {
        uint64_t div = 1;
        for (int k = 1; k < lv; k++)
            div *= ptrs;
        blk = ind_entry(fs, blk, (uint32_t)(rel / div % ptrs));
    }
    return blk;
}

static int ind_set(ext4_inode_t *ei, uint32_t lblk, uint64_t pblk) {
    ext4_fs_t *fs   = ei->fs;
    uint32_t   ptrs = fs->block_size / 4;
    uint64_t   rel;
    int        levels = ind_split(fs, lblk, &rel);

    if (levels < 0 || (pblk >> 32))
        return -EFBIG;
    if (levels == 0) {
        ei->disk.i_block[rel] = (uint32_t)pblk;
        return 0;
    }

    if (!ei->disk.i_block[11 + levels]) {
        uint64_t nb = ext4_alloc_block(fs, pblk, true);
        if (!nb)
            return -ENOSPC;
        ei->disk.i_block[11 + levels] = (uint32_t)nb;
        ext4_iblocks_add(ei, 1);
    }

    uint32_t *tab = kmalloc(fs->block_size);
    if (!tab)
        return -ENOMEM;

    uint32_t blk = ei->disk.i_block[11 + levels];
    int      r   = 0;
    for (int lv = levels; lv >= 1; lv--) {
        uint64_t div = 1;
        for (int k = 1; k < lv; k++)
            div *= ptrs;
        uint32_t i = (uint32_t)(rel / div % ptrs);

        if ((r = ext4_read_block(fs, blk, tab)) < 0)
            break;
        if (lv == 1) {
            tab[i] = (uint32_t)pblk;
            r = ext4_write_block(fs, blk, tab);
            break;
        }
        if (!tab[i]) {
            uint64_t nb = ext4_alloc_block(fs, pblk, true);
            if (!nb) {
                r = -ENOSPC;
                break;
            }
            tab[i] = (uint32_t)nb;
            if ((r = ext4_write_block(fs, blk, tab)) < 0)
                break;
            ext4_iblocks_add(ei, 1);
        }
        blk = tab[i];
    }
    kfree(tab);
    return r;
}

// frees whats at or past from under blk, true when nothing in it is left
static bool ind_trunc(ext4_inode_t *ei, uint32_t blk, int level, uint64_t base, uint64_t from) {
    ext4_fs_t *fs   = ei->fs;
    uint32_t   ptrs = fs->block_size / 4;
    uint64_t   span = 1;
    for (int k = 1; k < level; k++)
        span *= ptrs;

    uint32_t *tab = kmalloc(fs->block_size);
    if (!tab)
        return false;
    if (ext4_read_block(fs, blk, tab) < 0) {
        kfree(tab);
        return false;
    }

    bool used = false, dirty = false;
    for (uint32_t i = 0; i < ptrs; i++) {
        if (!tab[i])
            continue;
        uint64_t start = base + i * span;
        if (start + span <= from) {
            used = true;
            continue;
        }
        if (level == 1 || ind_trunc(ei, tab[i], level - 1, start, from)) {
            ext4_free_blocks(fs, tab[i], 1);
            ext4_iblocks_add(ei, -1);
            tab[i] = 0;
            dirty  = true;
        } else {
            used = true;
        }
    }
    if (dirty && used)
        ext4_write_block(fs, blk, tab);
    kfree(tab);
    return !used;
}

static void ind_truncate(ext4_inode_t *ei, uint32_t from) {
    ext4_fs_t *fs   = ei->fs;
    uint64_t   ptrs = fs->block_size / 4;

    for (uint32_t i = from; i < 12; i++) {
        if (ei->disk.i_block[i]) {
            ext4_free_blocks(fs, ei->disk.i_block[i], 1);
            ext4_iblocks_add(ei, -1);
            ei->disk.i_block[i] = 0;
        }
    }

    uint64_t base = 12, span = ptrs;
    for (int lv = 1; lv <= 3; lv++) {
        uint32_t top = ei->disk.i_block[11 + lv];
        if (top && base + span > from && ind_trunc(ei, top, lv, base, from)) {
            ext4_free_blocks(fs, top, 1);
            ext4_iblocks_add(ei, -1);
            ei->disk.i_block[11 + lv] = 0;
        }
        base += span;
        span *= ptrs;
    }
}

// the public side
int ext4_map(ext4_inode_t *ei, uint32_t lblk, uint64_t *pblk, uint32_t *len, bool *unwritten) {
    if (ei->disk.i_flags & EXT4_EXTENTS_FL)
        return ext_map(ei, lblk, pblk, len, unwritten);
    *pblk      = ind_get(ei, lblk);
    *len       = 1;
    *unwritten = false;
    return 0;
}

int ext4_map_set(ext4_inode_t *ei, uint32_t lblk, uint64_t pblk, uint32_t count) {
    int r = 0;

    if (ei->disk.i_flags & EXT4_EXTENTS_FL) {
        while (count && r == 0) {
            uint32_t n = count > EXT4_EXT_INIT_MAX ? EXT4_EXT_INIT_MAX : count;
            r = ext_insert(ei, ext_make(lblk, n, pblk, false));
            if (r == 0)
                ext4_iblocks_add(ei, n);
            lblk  += n;
            pblk  += n;
            count -= n;
        }
    } else {
        for (; count && r == 0; count--, lblk++, pblk++) {
            r = ind_set(ei, lblk, pblk);
            if (r == 0)
                ext4_iblocks_add(ei, 1);
        }
    }

    int w = ext4_write_inode(ei->fs, ei->ino, &ei->disk, false);
    return r < 0 ? r : w;
}

// lblk sits in an unwritten (fallocated) extent and just got real data, cut it out
int ext4_map_mark_written(ext4_inode_t *ei, uint32_t lblk) {
    if (!(ei->disk.i_flags & EXT4_EXTENTS_FL))
        return 0;

    ext_path_t p[EXT_MAX_DEPTH + 1];
    int depth;
    int r = path_find(ei, lblk, p, &depth);
    if (r < 0)
        return r;

    ext4_extent_t *e = EXT(p[depth].buf);
    int            i = p[depth].slot;
    if (i < 0 || !ext_unwritten(&e[i]) || lblk - e[i].ee_block >= ext_len(&e[i])) {
        path_free(p, depth);
        return 0;
    }

    uint32_t s   = e[i].ee_block;
    uint32_t n   = ext_len(&e[i]);
    uint64_t ps  = ext_start(&e[i]);
    uint32_t off = lblk - s;

    ext4_extent_t ins[2];
    int           nins = 0;

    if (n == 1) {
        ext_len_set(&e[i], 1, false);
    } else if (off == 0) {
        // writing through fallocated space front to back, grow the written extent before it
        if (i > 0 && !ext_unwritten(&e[i - 1]) && e[i - 1].ee_block + ext_len(&e[i - 1]) == s &&
            ext_start(&e[i - 1]) + ext_len(&e[i - 1]) == ps && ext_len(&e[i - 1]) < EXT4_EXT_INIT_MAX) {
            ext_len_set(&e[i - 1], ext_len(&e[i - 1]) + 1, false);
            e[i] = ext_make(s + 1, n - 1, ps + 1, true);
        } else {
            ext_len_set(&e[i], 1, false);
            ins[nins++] = ext_make(s + 1, n - 1, ps + 1, true);
        }
    } else {
        ext_len_set(&e[i], off, true);
        ins[nins++] = ext_make(lblk, 1, ps + off, false);
        if (off + 1 < n)
            ins[nins++] = ext_make(lblk + 1, n - off - 1, ps + off + 1, true);
    }

    r = node_write(ei, &p[depth]);
    path_free(p, depth);
    for (int k = 0; k < nins && r == 0; k++)
        r = ext_insert(ei, ins[k]);

    int w = ext4_write_inode(ei->fs, ei->ino, &ei->disk, false);
    return r < 0 ? r : w;
}

int ext4_map_truncate(ext4_inode_t *ei, uint32_t from) {
    if (ei->disk.i_flags & EXT4_EXTENTS_FL) {
        ext4_extent_header_t *root = EH(ei->disk.i_block);
        if (!ext_header_ok(root, root->eh_depth, EXT_ROOT_MAX) || root->eh_depth > EXT_MAX_DEPTH)
            return -EIO;
        if (ext_trunc_node(ei, (uint8_t *)ei->disk.i_block, root->eh_depth, from) == 0)
            root->eh_depth = 0;
    } else {
        ind_truncate(ei, from);
    }
    return ext4_write_inode(ei->fs, ei->ino, &ei->disk, false);
}

void ext4_map_init(ext4_inode_t *ei) {
    memset(ei->disk.i_block, 0, sizeof(ei->disk.i_block));
    if (!(ei->fs->sb.s_feature_incompat & EXT4_INCOMPAT_EXTENTS))
        return;

    ext4_extent_header_t *h = EH(ei->disk.i_block);
    h->eh_magic = EXT4_EXT_MAGIC;
    h->eh_max   = EXT_ROOT_MAX;
    ei->disk.i_flags |= EXT4_EXTENTS_FL;
}
