#include "ext4_priv.h"

#include <fs/vfs.h>
#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <ACPI/acpi_time.h>
#include <user/errno.h>

static const INodeOps_t ext4_dir_ops;
static const INodeOps_t ext4_file_ops;
static const INodeOps_t ext4_link_ops;

#define EXT4_READ_RUN   64

#define INCOMPAT_OK  (EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_RECOVER | EXT4_INCOMPAT_META_BG | \
                      EXT4_INCOMPAT_EXTENTS | EXT4_INCOMPAT_64BIT | EXT4_INCOMPAT_MMP | \
                      EXT4_INCOMPAT_FLEX_BG | EXT4_INCOMPAT_EA_INODE | EXT4_INCOMPAT_CSUM_SEED | \
                      EXT4_INCOMPAT_LARGEDIR | EXT4_INCOMPAT_CASEFOLD)
// these mount, but only to read
#define INCOMPAT_RO  (EXT4_INCOMPAT_RECOVER | EXT4_INCOMPAT_MMP | EXT4_INCOMPAT_EA_INODE | EXT4_INCOMPAT_CASEFOLD)
#define RO_COMPAT_RW (EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE | EXT4_RO_COMPAT_BTREE_DIR | \
                      EXT4_RO_COMPAT_HUGE_FILE | EXT4_RO_COMPAT_GDT_CSUM | EXT4_RO_COMPAT_DIR_NLINK | \
                      EXT4_RO_COMPAT_EXTRA_ISIZE | EXT4_RO_COMPAT_METADATA_CSUM)

static uint8_t ft_of(uint16_t mode) {
    switch (mode & EXT4_S_IFMT) {
        case EXT4_S_IFDIR: return EXT4_FT_DIR;
        case EXT4_S_IFLNK: return EXT4_FT_SYMLINK;
        default:           return EXT4_FT_REG_FILE;
    }
}

static INode_t *make_vfs_inode(ext4_fs_t *fs, uint32_t ino, const ext4_disk_inode_t *di,
                               INode_t *parent, const char *name, size_t namelen) {
    ext4_inode_t *ei = kmalloc(sizeof(*ei));
    if (!ei)
        return NULL;
    ei->fs  = fs;
    ei->ino = ino;
    memcpy(&ei->disk, di, sizeof(*di));

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) {
        kfree(ei);
        return NULL;
    }
    memset(inode, 0, sizeof(*inode));

    switch (di->i_mode & EXT4_S_IFMT) {
        case EXT4_S_IFDIR:
            inode->type = INODE_DIRECTORY;
            inode->ops  = &ext4_dir_ops;
            break;
        case EXT4_S_IFLNK:
            inode->type = INODE_SYMLINK;
            inode->ops  = &ext4_link_ops;
            break;
        default:
            inode->type = INODE_FILE;
            inode->ops  = &ext4_file_ops;
            break;
    }
    inode->internal_data = ei;
    inode->parent        = parent;
    inode->shared        = 1;

    if (name) {
        size_t n = namelen < sizeof(inode->name) - 1 ? namelen : sizeof(inode->name) - 1;
        memcpy(inode->name, name, n);
        inode->name[n] = '\0';
    }
    return inode;
}

// file data
static long file_read(ext4_inode_t *ei, void *buf, size_t count, size_t offset) {
    ext4_fs_t *fs   = ei->fs;
    uint64_t   size = ext4_isize(&ei->disk);

    if (offset >= size)
        return 0;
    if (count > size - offset)
        count = (size_t)(size - offset);

    uint8_t *bb    = NULL;
    size_t   total = 0;
    while (total < count) {
        uint64_t pos  = offset + total;
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        size_t   boff = pos % fs->block_size;
        size_t   want = count - total;
        uint64_t pblk;
        uint32_t len;
        bool     uw;

        if (ext4_map(ei, lblk, &pblk, &len, &uw) < 0)
            break;
        if (uw)
            pblk = 0;

        if (!pblk) {
            size_t n = (size_t)len * fs->block_size - boff;
            if (n > want)
                n = want;
            memset((uint8_t *)buf + total, 0, n);
            total += n;
            continue;
        }

        if (boff == 0 && want >= fs->block_size) {
            uint32_t cap = (uint32_t)(want / fs->block_size);
            if (cap > EXT4_READ_RUN)
                cap = EXT4_READ_RUN;
            uint32_t run = len < cap ? len : cap;

            // the indirect map gives one block at a time
            while (run < cap) {
                uint64_t p2;
                uint32_t l2;
                bool     u2;
                if (ext4_map(ei, lblk + run, &p2, &l2, &u2) < 0 || u2 || p2 != pblk + run)
                    break;
                run += l2 < cap - run ? l2 : cap - run;
            }
            if (ext4_read_bytes(fs, pblk * fs->block_size, (uint8_t *)buf + total,
                                (size_t)run * fs->block_size) < 0)
                break;
            total += (size_t)run * fs->block_size;
            continue;
        }

        if (!bb && !(bb = kmalloc(fs->block_size)))
            break;
        if (ext4_read_block(fs, pblk, bb) < 0)
            break;
        size_t chunk = fs->block_size - boff;
        if (chunk > want)
            chunk = want;
        memcpy((uint8_t *)buf + total, bb + boff, chunk);
        total += chunk;
    }
    kfree(bb);
    return (long)total;
}

static long file_write(ext4_inode_t *ei, const void *buf, size_t count, size_t offset) {
    ext4_fs_t *fs = ei->fs;

    if (fs->ro)
        return -EROFS;
    if (!count)
        return 0;
    if ((offset + count - 1) / fs->block_size > UINT32_MAX)
        return -EFBIG;

    uint8_t *bb = kmalloc(fs->block_size);
    if (!bb)
        return -ENOMEM;

    // new blocks get mapped a run at a time, one extent instead of thousands of merges
    uint32_t run_l = 0, run_n = 0;
    uint64_t run_p = 0;
    uint64_t last  = 0;
    size_t   total = 0;
    int      err   = 0;
    bool     ext   = ei->disk.i_flags & EXT4_EXTENTS_FL;

    while (total < count) {
        uint64_t pos   = offset + total;
        uint32_t lblk  = (uint32_t)(pos / fs->block_size);
        size_t   boff  = pos % fs->block_size;
        size_t   chunk = fs->block_size - boff;
        if (chunk > count - total)
            chunk = count - total;

        uint64_t pblk;
        uint32_t len;
        bool     uw;
        if ((err = ext4_map(ei, lblk, &pblk, &len, &uw)) < 0)
            break;

        bool fresh = uw;
        if (!pblk) {
            if (!last && lblk) {
                uint64_t prev;
                bool     pu;
                if (ext4_map(ei, lblk - 1, &prev, &len, &pu) == 0 && prev)
                    last = prev;
            }
            pblk = ext4_alloc_block(fs, last ? last + 1 : 0, false);
            if (!pblk) {
                err = -ENOSPC;
                break;
            }
            fresh = true;

            if (run_n && run_l + run_n == lblk && run_p + run_n == pblk && (!ext || run_n < EXT4_EXT_INIT_MAX)) {
                run_n++;
            } else {
                if (run_n && (err = ext4_map_set(ei, run_l, run_p, run_n)) < 0) {
                    ext4_free_blocks(fs, run_p, run_n);
                    ext4_free_blocks(fs, pblk, 1);
                    total = (uint64_t)run_l * fs->block_size > offset ? (uint64_t)run_l * fs->block_size - offset : 0;
                    run_n = 0;
                    break;
                }
                run_l = lblk;
                run_p = pblk;
                run_n = 1;
            }
        }

        if (boff || chunk < fs->block_size) {
            if (fresh)
                memset(bb, 0, fs->block_size);
            else if ((err = ext4_read_block(fs, pblk, bb)) < 0)
                break;
        }
        memcpy(bb + boff, (const uint8_t *)buf + total, chunk);
        if ((err = ext4_write_block(fs, pblk, bb)) < 0)
            break;
        if (uw && (err = ext4_map_mark_written(ei, lblk)) < 0)
            break;

        total += chunk;
        last   = pblk;
    }

    if (run_n && ext4_map_set(ei, run_l, run_p, run_n) < 0) {
        ext4_free_blocks(fs, run_p, run_n);
        uint64_t at = (uint64_t)run_l * fs->block_size;
        if (at < offset + total)
            total = at > offset ? at - offset : 0;
        err = -ENOSPC;
    }
    kfree(bb);

    uint32_t now   = (uint32_t)clock_now();
    uint64_t end   = offset + total;
    ei->disk.i_mtime = ei->disk.i_ctime = now;
    if (end > ext4_isize(&ei->disk)) {
        ext4_isize_set(&ei->disk, end);
        if (end > 0x7FFFFFFF && !(fs->sb.s_feature_ro_compat & EXT4_RO_COMPAT_LARGE_FILE)) {
            fs->sb.s_feature_ro_compat |= EXT4_RO_COMPAT_LARGE_FILE;
            fs->sb_dirty = true;
        }
    }
    ext4_write_inode(fs, ei->ino, &ei->disk, false);
    ext4_alloc_flush(fs);

    return total ? (long)total : err;
}

// the inode is done with, everything it owns goes back
static void release_inode(ext4_inode_t *ei) {
    ext4_fs_t *fs     = ei->fs;
    bool       is_dir = ext4_is_dir(&ei->disk);

    if (!ext4_fast_link(fs, &ei->disk))
        ext4_map_truncate(ei, 0);

    // an xattr block can be shared between inodes
    uint64_t acl = ext4_file_acl(&ei->disk);
    uint8_t *buf = acl ? kmalloc(fs->block_size) : NULL;
    if (buf && ext4_read_block(fs, acl, buf) == 0 && *(uint32_t *)buf == EXT4_XATTR_MAGIC) {
        uint32_t *refs = (uint32_t *)(buf + 4);
        if (*refs <= 1) {
            ext4_free_blocks(fs, acl, 1);
        } else {
            (*refs)--;
            ext4_xattr_csum_set(fs, acl, buf);
            ext4_write_block(fs, acl, buf);
        }
        ei->disk.i_file_acl_lo   = 0;
        ei->disk.i_file_acl_high = 0;
    }
    kfree(buf);

    memset(ei->disk.i_block, 0, sizeof(ei->disk.i_block));
    ext4_isize_set(&ei->disk, 0);
    ei->disk.i_blocks_lo     = 0;
    ei->disk.i_blocks_high   = 0;
    ei->disk.i_links_count   = 0;
    ei->disk.i_dtime         = (uint32_t)clock_now();
    ext4_write_inode(fs, ei->ino, &ei->disk, false);
    ext4_free_inode(fs, ei->ino, is_dir);
}

// inode ops
static long ext4_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    return file_read(inode->internal_data, buf, count, offset);
}

static long ext4_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    return file_write(inode->internal_data, buf, count, offset);
}

static size_t ext4_size(INode_t *inode) {
    ext4_inode_t *ei = inode->internal_data;
    return ei ? (size_t)ext4_isize(&ei->disk) : 0;
}

static int ext4_truncate(INode_t *inode, size_t new_size) {
    ext4_inode_t *ei = inode->internal_data;
    ext4_fs_t    *fs = ei->fs;
    uint64_t      size = ext4_isize(&ei->disk);

    if (fs->ro)
        return -EROFS;
    if (new_size == size)
        return 0;

    if (new_size < size) {
        uint32_t keep = (uint32_t)((new_size + fs->block_size - 1) / fs->block_size);
        int r = ext4_map_truncate(ei, keep);
        if (r < 0) {
            ext4_alloc_flush(fs);
            return r;
        }

        // the rest of the last block has to read as zeros if the file grows again
        size_t tail = new_size % fs->block_size;
        uint64_t pblk;
        uint32_t len;
        bool     uw;
        if (tail && ext4_map(ei, keep - 1, &pblk, &len, &uw) == 0 && pblk && !uw) {
            uint8_t *bb = kmalloc(fs->block_size);
            if (bb && ext4_read_block(fs, pblk, bb) == 0) {
                memset(bb + tail, 0, fs->block_size - tail);
                ext4_write_block(fs, pblk, bb);
            }
            kfree(bb);
        }
    }
    // growing just leaves a hole

    ext4_isize_set(&ei->disk, new_size);
    ei->disk.i_mtime = ei->disk.i_ctime = (uint32_t)clock_now();
    int r = ext4_write_inode(fs, ei->ino, &ei->disk, false);
    ext4_alloc_flush(fs);
    return r;
}

static int ext4_statfs(INode_t *root, uint64_t *block_size, uint64_t *total, uint64_t *free) {
    ext4_inode_t *ei = root->internal_data;
    if (!ei)
        return -EINVAL;
    ext4_fs_t *fs = ei->fs;
    *block_size = fs->block_size;
    *total      = fs->blocks_count;
    *free       = fs->sb.s_free_blocks_count_lo;
    if (fs->sb.s_feature_incompat & EXT4_INCOMPAT_64BIT)
        *free |= (uint64_t)fs->sb.s_free_blocks_count_hi << 32;
    return 0;
}

static int ext4_label(INode_t *root, char *out, size_t size) {
    ext4_inode_t *ei = root->internal_data;
    if (!ei || !size)
        return -EINVAL;
    const char *v = ei->fs->sb.s_volume_name;
    size_t n = 0;
    for (; n < sizeof(ei->fs->sb.s_volume_name) && n < size - 1 && v[n]; n++)
        out[n] = v[n];
    out[n] = '\0';
    return n ? 0 : -ENOENT;
}

static void ext4_times(INode_t *inode, int64_t *atime, int64_t *mtime, int64_t *ctime) {
    ext4_inode_t *ei = inode->internal_data;
    if (!ei)
        return;
    *atime = ei->disk.i_atime;
    *mtime = ei->disk.i_mtime;
    *ctime = ei->disk.i_ctime;
}

static void ext4_drop(INode_t *inode) {
    if (!inode || !inode->internal_data)
        return;
    kfree(inode->internal_data);
    inode->internal_data = NULL;
}

static int ext4_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result) {
    ext4_inode_t *ei = dir->internal_data;

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

    uint32_t ino;
    if (ext4_dir_find(ei, name, namelen, &ino, NULL) < 0)
        return -ENOENT;

    ext4_disk_inode_t di;
    if (ext4_read_inode(ei->fs, ino, &di) < 0)
        return -EIO;

    INode_t *child = make_vfs_inode(ei->fs, ino, &di, dir, name, namelen);
    if (!child)
        return -ENOMEM;
    *result = child;
    return 0;
}

static int ext4_readdir(INode_t *dir, size_t index, INode_t **result) {
    ext4_inode_t *ei = dir->internal_data;

    char     name[256];
    uint32_t ino;
    if (ext4_dir_nth(ei, index, name, &ino) < 0)
        return -ENOENT;

    ext4_disk_inode_t di;
    if (ext4_read_inode(ei->fs, ino, &di) < 0)
        return -EIO;

    INode_t *child = make_vfs_inode(ei->fs, ino, &di, dir, name, strlen(name));
    if (!child)
        return -ENOMEM;
    *result = child;
    return 0;
}

static int make_child(INode_t *parent, const char *name, size_t namelen, INode_t **result,
                      inode_type type, const char *target) {
    ext4_inode_t *pei = parent->internal_data;
    ext4_fs_t    *fs  = pei->fs;

    if (fs->ro)
        return -EROFS;
    if (!namelen || namelen > 255)
        return -ENAMETOOLONG;

    bool   is_dir  = type == INODE_DIRECTORY;
    bool   is_link = type == INODE_SYMLINK;
    size_t tlen    = is_link ? strlen(target) : 0;

    uint32_t ino = ext4_alloc_inode(fs, pei->ino, is_dir);
    if (!ino) {
        ext4_alloc_flush(fs);
        return -ENOSPC;
    }

    ext4_inode_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.fs  = fs;
    ci.ino = ino;

    ext4_disk_inode_t *di = &ci.disk;
    uint32_t now = (uint32_t)clock_now();
    di->i_mode        = is_dir ? (EXT4_S_IFDIR | 0755) : is_link ? (EXT4_S_IFLNK | 0777) : (EXT4_S_IFREG | 0644);
    di->i_links_count = is_dir ? 2 : 1;
    di->i_atime = di->i_ctime = di->i_mtime = di->i_crtime = now;
    di->i_generation  = now ^ (ino * 2654435761u);
    if (fs->inode_size >= sizeof(*di))
        di->i_extra_isize = sizeof(*di) - EXT4_GOOD_OLD_INODE;

    // short targets live in i_block, those never get the extents flag
    if (is_link && tlen < EXT4_FAST_LINK_MAX) {
        memcpy(di->i_block, target, tlen);
        ext4_isize_set(di, tlen);
    } else {
        ext4_map_init(&ci);
    }

    int r = ext4_write_inode(fs, ino, di, true);
    if (r == 0 && is_dir)
        r = ext4_dir_init(&ci, pei->ino);
    if (r == 0 && is_link && tlen >= EXT4_FAST_LINK_MAX && file_write(&ci, target, tlen, 0) != (long)tlen)
        r = -ENOSPC;
    if (r == 0)
        r = ext4_dir_add(pei, name, namelen, ino, ft_of(di->i_mode));
    if (r < 0) {
        release_inode(&ci);
        ext4_alloc_flush(fs);
        return r;
    }

    if (is_dir) {
        uint16_t links = pei->disk.i_links_count;
        if (links >= EXT4_LINK_MAX - 1 || links == 1)
            pei->disk.i_links_count = (fs->sb.s_feature_ro_compat & EXT4_RO_COMPAT_DIR_NLINK) ? 1 : links;
        else
            pei->disk.i_links_count++;
    }
    pei->disk.i_mtime = pei->disk.i_ctime = now;
    ext4_write_inode(fs, pei->ino, &pei->disk, false);
    ext4_alloc_flush(fs);

    INode_t *child = make_vfs_inode(fs, ino, di, parent, name, namelen);
    if (!child)
        return -ENOMEM;
    *result = child;
    return 0;
}

static int ext4_create(INode_t *parent, const char *name, size_t namelen, INode_t **result, inode_type type) {
    if (type == INODE_SYMLINK)
        return -EINVAL;
    return make_child(parent, name, namelen, result, type, NULL);
}

static int ext4_symlink(INode_t *dir, const char *name, size_t namelen, const char *target, INode_t **result) {
    return make_child(dir, name, namelen, result, INODE_SYMLINK, target);
}

static int ext4_unlink(INode_t *dir, const char *name, size_t namelen) {
    ext4_inode_t *dei = dir->internal_data;
    ext4_fs_t    *fs  = dei->fs;

    if (fs->ro)
        return -EROFS;

    uint32_t      ino;
    ext4_dirpos_t pos;
    if (ext4_dir_find(dei, name, namelen, &ino, &pos) < 0)
        return -ENOENT;

    ext4_inode_t target = { .fs = fs, .ino = ino };
    if (ext4_read_inode(fs, ino, &target.disk) < 0)
        return -EIO;

    bool is_dir = ext4_is_dir(&target.disk);
    if (is_dir && !ext4_dir_empty(&target))
        return -ENOTEMPTY;

    int r = ext4_dir_remove(dei, pos);
    if (r < 0)
        return r;

    uint32_t now = (uint32_t)clock_now();
    if (is_dir) {
        target.disk.i_links_count = 0;
        if (dei->disk.i_links_count > 2)
            dei->disk.i_links_count--;
    } else if (target.disk.i_links_count) {
        target.disk.i_links_count--;
    }
    dei->disk.i_mtime = dei->disk.i_ctime = now;
    ext4_write_inode(fs, dei->ino, &dei->disk, false);

    if (!target.disk.i_links_count) {
        release_inode(&target);
    } else {
        target.disk.i_ctime = now;
        ext4_write_inode(fs, ino, &target.disk, false);
    }
    ext4_alloc_flush(fs);
    return 0;
}

static int ext4_rename(INode_t *dir, const char *oldname, size_t oldlen, const char *newname, size_t newlen) {
    ext4_inode_t *dei = dir->internal_data;
    ext4_fs_t    *fs  = dei->fs;

    if (fs->ro)
        return -EROFS;
    if (!newlen || newlen > 255)
        return -ENAMETOOLONG;

    uint32_t      ino;
    ext4_dirpos_t pos;
    if (ext4_dir_find(dei, oldname, oldlen, &ino, &pos) < 0)
        return -ENOENT;

    int r = ext4_dir_rename(dei, pos, newname, newlen);
    if (r == -ENOSPC) {
        // add the new one before the old goes, a failure leaves the file where it was
        ext4_disk_inode_t di;
        r = ext4_read_inode(fs, ino, &di);
        if (r == 0)
            r = ext4_dir_add(dei, newname, newlen, ino, ft_of(di.i_mode));
        if (r == 0)
            r = ext4_dir_remove(dei, pos);
    }
    ext4_alloc_flush(fs);
    return r;
}

static long ext4_readlink(INode_t *inode, char *out, size_t size) {
    ext4_inode_t *ei  = inode->internal_data;
    uint64_t      len = ext4_isize(&ei->disk);

    if (ext4_fast_link(ei->fs, &ei->disk)) {
        if (len > size)
            len = size;
        memcpy(out, ei->disk.i_block, len);
        return (long)len;
    }
    return file_read(ei, out, len < size ? len : size, 0);
}

static const INodeOps_t ext4_link_ops = {
    .readlink = ext4_readlink,
    .size     = ext4_size,
    .drop     = ext4_drop,
    .times    = ext4_times,
};

static const INodeOps_t ext4_dir_ops = {
    .lookup  = ext4_lookup,
    .readdir = ext4_readdir,
    .create  = ext4_create,
    .unlink  = ext4_unlink,
    .rename  = ext4_rename,
    .drop    = ext4_drop,
    .times   = ext4_times,
    .statfs  = ext4_statfs,
    .label   = ext4_label,
    .symlink = ext4_symlink,
};

static const INodeOps_t ext4_file_ops = {
    .read     = ext4_read,
    .write    = ext4_write,
    .truncate = ext4_truncate,
    .size     = ext4_size,
    .drop     = ext4_drop,
    .times    = ext4_times,
};

// mounting
bool ext4_probe(INode_t *dev_inode) {
    ext4_superblock_t *sb = kmalloc(sizeof(*sb));
    if (!sb)
        return false;

    bool r = inode_read(dev_inode, sb, sizeof(*sb), EXT4_SUPERBLOCK_OFFSET) == (long)sizeof(*sb) &&
             sb->s_magic == EXT4_SUPER_MAGIC &&
             ((sb->s_feature_compat & EXT4_COMPAT_HAS_JOURNAL) ||
              (sb->s_feature_incompat & ~EXT4_INCOMPAT_FILETYPE) ||
              (sb->s_feature_ro_compat & ~(EXT4_RO_COMPAT_SPARSE_SUPER | EXT4_RO_COMPAT_LARGE_FILE |
                                           EXT4_RO_COMPAT_BTREE_DIR)));
    kfree(sb);
    return r;
}

static bool superblock_sane(const ext4_superblock_t *sb) {
    uint32_t bs = 1024u << (sb->s_log_block_size > 5 ? 0 : sb->s_log_block_size);
    if (sb->s_log_block_size > 5 || !sb->s_blocks_per_group || !sb->s_inodes_per_group ||
        !sb->s_inodes_count || sb->s_blocks_per_group > bs * 8 || sb->s_inodes_per_group > bs * 8 ||
        (sb->s_inodes_per_group & 7))
        return false;
    if (sb->s_rev_level >= 1 && (sb->s_inode_size < EXT4_GOOD_OLD_INODE ||
                                 (sb->s_inode_size & (sb->s_inode_size - 1)) || sb->s_inode_size > bs))
        return false;
    if ((sb->s_feature_incompat & EXT4_INCOMPAT_64BIT) &&
        (sb->s_desc_size < 64 || sb->s_desc_size > 1024 || (sb->s_desc_size & (sb->s_desc_size - 1))))
        return false;
    return true;
}

static void fs_free(ext4_fs_t *fs) {
    for (int i = 0; i < EXT4_MAP_CACHE; i++)
        kfree(fs->map_cache[i].data);
    kfree(fs->bm_buf);
    kfree(fs->gdt);
    kfree(fs);
}

int ext4_mount(INode_t *dev_inode, INode_t **root) {
    if (!dev_inode || !dev_inode->ops || !dev_inode->ops->read) {
        serial_printf(LOG_ERROR "ext4: null or invalid device inode\n");
        return -EIO;
    }

    bool replayed = false;
again:;
    ext4_fs_t *fs = kmalloc(sizeof(*fs));
    if (!fs)
        return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->dev      = dev_inode;
    fs->bm_group = EXT4_BM_NONE;

    ext4_superblock_t *sb = &fs->sb;
    if (inode_read(dev_inode, sb, sizeof(*sb), EXT4_SUPERBLOCK_OFFSET) != (long)sizeof(*sb) ||
        sb->s_magic != EXT4_SUPER_MAGIC) {
        serial_printf(LOG_ERROR "ext4: no superblock\n");
        fs_free(fs);
        return -EIO;
    }

    // a bad stick shouldnt take the kernel down, everything below divides by these
    if (!superblock_sane(sb)) {
        serial_printf(LOG_ERROR "ext4: superblock doesnt make sense, not mounting\n");
        fs_free(fs);
        return -EIO;
    }

    uint32_t incompat  = sb->s_feature_incompat;
    uint32_t ro_compat = sb->s_feature_ro_compat;
    if ((incompat & ~INCOMPAT_OK) || (ro_compat & EXT4_RO_COMPAT_BIGALLOC)) {
        serial_printf(LOG_ERROR "ext4: unsupported features incompat=0x%x ro_compat=0x%x\n",
                      incompat & ~INCOMPAT_OK, ro_compat & EXT4_RO_COMPAT_BIGALLOC);
        fs_free(fs);
        return -EIO;
    }

    fs->block_size       = 1024u << sb->s_log_block_size;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->first_data_block = sb->s_first_data_block;
    fs->total_inodes     = sb->s_inodes_count;
    fs->inode_size       = sb->s_rev_level >= 1 ? sb->s_inode_size : EXT4_GOOD_OLD_INODE;
    fs->desc_size        = (incompat & EXT4_INCOMPAT_64BIT) ? sb->s_desc_size : 32;
    fs->blocks_count     = sb->s_blocks_count_lo;
    if (incompat & EXT4_INCOMPAT_64BIT)
        fs->blocks_count |= (uint64_t)sb->s_blocks_count_hi << 32;
    fs->csum     = ro_compat & EXT4_RO_COMPAT_METADATA_CSUM;
    fs->gdt_csum = !fs->csum && (ro_compat & EXT4_RO_COMPAT_GDT_CSUM);

    if (fs->blocks_count <= fs->first_data_block) {
        serial_printf(LOG_ERROR "ext4: superblock doesnt make sense, not mounting\n");
        fs_free(fs);
        return -EIO;
    }
    uint64_t groups = (fs->blocks_count - fs->first_data_block + fs->blocks_per_group - 1) / fs->blocks_per_group;
    if (groups > UINT32_MAX / fs->inodes_per_group || fs->total_inodes > groups * fs->inodes_per_group) {
        serial_printf(LOG_ERROR "ext4: group count doesnt add up, not mounting\n");
        fs_free(fs);
        return -EIO;
    }
    fs->num_groups = (uint32_t)groups;

    if (fs->csum)
        fs->csum_seed = (incompat & EXT4_INCOMPAT_CSUM_SEED) ? sb->s_checksum_seed
                                                            : ext4_crc32c(~0u, sb->s_uuid, sizeof(sb->s_uuid));

    bool want_replay = (incompat & EXT4_INCOMPAT_RECOVER) && !replayed &&
                       (sb->s_feature_compat & EXT4_COMPAT_HAS_JOURNAL) &&
                       !(incompat & EXT4_INCOMPAT_MMP) && dev_inode->ops->write;
    if (incompat & INCOMPAT_RO) {
        if (!want_replay)
            serial_printf(LOG_WARN "ext4: %s, mounting read only\n",
                          (incompat & EXT4_INCOMPAT_RECOVER) ? "journal needs replaying (unclean shutdown)"
                                                             : "has features we cant write");
        fs->ro = true;
    }
    if (ro_compat & ~RO_COMPAT_RW) {
        serial_printf(LOG_WARN "ext4: ro_compat 0x%x we cant keep up, mounting read only\n", ro_compat & ~RO_COMPAT_RW);
        fs->ro = true;
    }
    if (fs->csum && sb->s_checksum_type != 1) {
        serial_printf(LOG_WARN "ext4: unknown checksum type %u, mounting read only\n", sb->s_checksum_type);
        fs->ro = true;
    }
    if (!ext4_sb_csum_ok(fs)) {
        serial_printf(LOG_WARN "ext4: superblock checksum is wrong, mounting read only\n");
        fs->ro = true;
    }
    if (sb->s_state & EXT4_STATE_ERROR)
        serial_printf(LOG_WARN "ext4: fs is marked as having errors, run e2fsck on it\n");

    if (ext4_gdt_load(fs) < 0) {
        serial_printf(LOG_ERROR "ext4: couldnt read the group descriptors\n");
        fs_free(fs);
        return -EIO;
    }
    // linux didnt get to unmount this. put the journal back on disk and start the mount over,
    // the superblock and group descriptors we have loaded may be older than whats in the log
    // (MMP means another machine could have it open, so thats left read only)
    if (want_replay) {
        serial_printf(LOG_INFO "ext4: not unmounted cleanly, replaying the journal\n");
        int r = ext4_journal_replay(fs);
        fs_free(fs);
        // on failure RECOVER is still set, so the second go mounts it read only
        if (r < 0)
            serial_printf(LOG_WARN "ext4: journal replay failed (%d), mounting read only\n", r);
        replayed = true;
        goto again;
    }

    uint32_t bad = 0;
    for (uint32_t g = 0; g < fs->num_groups; g++) {
        if (!ext4_gd_csum_ok(fs, g))
            bad++;
        if (ext4_gd_inode_table(fs, g) >= fs->blocks_count) {
            serial_printf(LOG_ERROR "ext4: group %u inode table is off the disk, not mounting\n", g);
            fs_free(fs);
            return -EIO;
        }
    }
    if (bad) {
        serial_printf(LOG_WARN "ext4: %u group descriptor checksums wrong, mounting read only\n", bad);
        fs->ro = true;
    }

    ext4_disk_inode_t root_di;
    if (ext4_read_inode(fs, EXT4_ROOT_INO, &root_di) < 0 || !ext4_is_dir(&root_di)) {
        serial_printf(LOG_ERROR "ext4: root inode isnt a directory\n");
        fs_free(fs);
        return -EIO;
    }

    INode_t *root_inode = make_vfs_inode(fs, EXT4_ROOT_INO, &root_di, NULL, NULL, 0);
    if (!root_inode) {
        fs_free(fs);
        return -ENOMEM;
    }

    serial_printf(LOG_OK "ext4: mounted%s - %lu blocks of %u, %u groups, %u inodes of %u%s%s\n",
                  fs->ro ? " read only" : "", fs->blocks_count, fs->block_size, fs->num_groups,
                  fs->total_inodes, fs->inode_size, fs->csum ? ", metadata_csum" : "",
                  (incompat & EXT4_INCOMPAT_EXTENTS) ? ", extents" : "");

    *root = root_inode;
    return 0;
}
