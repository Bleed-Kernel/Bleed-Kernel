// directories. htree dirs read fine as plain ones
#include "ext4_priv.h"

#include <mm/kalloc.h>
#include <string.h>
#include <user/errno.h>

#define REC_LEN(namelen) ((uint16_t)((sizeof(ext4_dirent_t) + (namelen) + 3) & ~3u))

static uint32_t dir_limit(ext4_fs_t *fs) {
    return fs->csum ? fs->block_size - sizeof(ext4_dir_tail_t) : fs->block_size;
}

static bool dirent_ok(ext4_fs_t *fs, const ext4_dirent_t *de, uint32_t off) {
    return de->rec_len >= sizeof(ext4_dirent_t) && !(de->rec_len & 3) &&
           off + de->rec_len <= fs->block_size && sizeof(ext4_dirent_t) + de->name_len <= de->rec_len;
}

static void tail_init(ext4_fs_t *fs, uint8_t *block) {
    if (!fs->csum)
        return;
    ext4_dir_tail_t *t = (ext4_dir_tail_t *)(block + fs->block_size - sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->det_rec_len = sizeof(*t);
    t->det_ft      = EXT4_DIR_TAIL_FT;
}

static uint32_t dir_blocks(ext4_inode_t *dir) {
    return (uint32_t)((ext4_isize(&dir->disk) + dir->fs->block_size - 1) / dir->fs->block_size);
}

static uint64_t dir_pblk(ext4_inode_t *dir, uint32_t lblk) {
    uint64_t pblk;
    uint32_t len;
    bool     uw;
    if (ext4_map(dir, lblk, &pblk, &len, &uw) < 0 || uw)
        return 0;
    return pblk;
}

static int dir_write_block(ext4_inode_t *dir, uint64_t pblk, uint8_t *buf) {
    ext4_dirblock_csum_set(dir, buf);
    return ext4_write_block(dir->fs, pblk, buf);
}

void ext4_dir_iterate(ext4_inode_t *dir, ext4_dirent_cb cb, void *ctx) {
    ext4_fs_t *fs  = dir->fs;
    uint8_t   *buf = kmalloc(fs->block_size);
    if (!buf)
        return;

    uint32_t n = dir_blocks(dir);
    for (uint32_t lb = 0; lb < n; lb++) {
        uint64_t pblk = dir_pblk(dir, lb);
        if (!pblk || ext4_read_block(fs, pblk, buf) < 0)
            continue;

        uint32_t off = 0;
        while (off + sizeof(ext4_dirent_t) <= fs->block_size) {
            ext4_dirent_t *de = (ext4_dirent_t *)(buf + off);
            if (!dirent_ok(fs, de, off))
                break;
            if (de->inode && de->name_len) {
                ext4_dirpos_t pos = { lb, off };
                if (cb(de, (const char *)(de + 1), pos, ctx)) {
                    kfree(buf);
                    return;
                }
            }
            off += de->rec_len;
        }
    }
    kfree(buf);
}

typedef struct {
    const char    *name;
    size_t         len;
    uint32_t       ino;
    ext4_dirpos_t  pos;
} find_ctx_t;

static bool find_cb(const ext4_dirent_t *de, const char *name, ext4_dirpos_t pos, void *ctx) {
    find_ctx_t *fc = ctx;
    if (de->name_len != fc->len || memcmp(name, fc->name, fc->len))
        return false;
    fc->ino = de->inode;
    fc->pos = pos;
    return true;
}

int ext4_dir_find(ext4_inode_t *dir, const char *name, size_t len, uint32_t *ino, ext4_dirpos_t *pos) {
    find_ctx_t fc = { name, len, 0, { 0, 0 } };
    ext4_dir_iterate(dir, find_cb, &fc);
    if (!fc.ino)
        return -ENOENT;
    *ino = fc.ino;
    if (pos)
        *pos = fc.pos;
    return 0;
}

typedef struct {
    size_t    target;
    size_t    at;
    char     *name;
    uint32_t  ino;
} nth_ctx_t;

static bool nth_cb(const ext4_dirent_t *de, const char *name, ext4_dirpos_t pos, void *ctx) {
    (void)pos;
    nth_ctx_t *nc = ctx;
    if ((de->name_len == 1 && name[0] == '.') || (de->name_len == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    if (nc->at++ != nc->target)
        return false;
    nc->ino = de->inode;
    if (nc->name) {
        memcpy(nc->name, name, de->name_len);
        nc->name[de->name_len] = '\0';
    }
    return true;
}

// name needs 256 bytes, . and .. dont count
int ext4_dir_nth(ext4_inode_t *dir, size_t index, char *name, uint32_t *ino) {
    nth_ctx_t nc = { index, 0, name, 0 };
    ext4_dir_iterate(dir, nth_cb, &nc);
    if (!nc.ino)
        return -ENOENT;
    *ino = nc.ino;
    return 0;
}

bool ext4_dir_empty(ext4_inode_t *dir) {
    uint32_t ino;
    return ext4_dir_nth(dir, 0, NULL, &ino) < 0;
}

static int dir_deindex(ext4_inode_t *dir) {
    ext4_fs_t *fs = dir->fs;
    if (!(dir->disk.i_flags & EXT4_INDEX_FL))
        return 0;

    // with checksums the index blocks have no dirent tail, they need one to pass as plain blocks
    if (fs->csum) {
        uint8_t *buf = kmalloc(fs->block_size);
        if (!buf)
            return -ENOMEM;

        uint32_t limit = dir_limit(fs);
        uint32_t n     = dir_blocks(dir);
        for (uint32_t lb = 0; lb < n; lb++) {
            uint64_t pblk = dir_pblk(dir, lb);
            if (!pblk || ext4_read_block(fs, pblk, buf) < 0)
                continue;

            bool           changed = false;
            ext4_dirent_t *de      = (ext4_dirent_t *)buf;
            if (lb == 0) {
                // the root is . then a .. that runs over the index to the end of the block
                uint32_t at = de->rec_len;
                ext4_dirent_t *dd = (ext4_dirent_t *)(buf + at);
                if (dirent_ok(fs, de, 0) && at + sizeof(*dd) <= limit && dirent_ok(fs, dd, at) &&
                    at + dd->rec_len > limit) {
                    dd->rec_len = (uint16_t)(limit - at);
                    changed     = true;
                }
            } else if (!de->inode && de->rec_len == fs->block_size) {
                de->rec_len = (uint16_t)limit;
                changed     = true;
            }

            if (changed) {
                tail_init(fs, buf);
                dir_write_block(dir, pblk, buf);
            }
        }
        kfree(buf);
    }

    dir->disk.i_flags &= ~EXT4_INDEX_FL;
    return ext4_write_inode(fs, dir->ino, &dir->disk, false);
}

int ext4_dir_add(ext4_inode_t *dir, const char *name, size_t len, uint32_t ino, uint8_t ft) {
    ext4_fs_t *fs = dir->fs;
    if (!len || len > 255)
        return -ENAMETOOLONG;

    int r = dir_deindex(dir);
    if (r < 0)
        return r;

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf)
        return -ENOMEM;

    uint16_t need  = REC_LEN(len);
    uint32_t limit = dir_limit(fs);
    uint32_t n     = dir_blocks(dir);
    uint64_t last  = 0;

    for (uint32_t lb = 0; lb < n; lb++) {
        uint64_t pblk = dir_pblk(dir, lb);
        if (!pblk || ext4_read_block(fs, pblk, buf) < 0)
            continue;
        last = pblk;

        uint32_t off = 0;
        while (off + sizeof(ext4_dirent_t) <= limit) {
            ext4_dirent_t *de = (ext4_dirent_t *)(buf + off);
            if (!dirent_ok(fs, de, off) || off + de->rec_len > limit)
                break;

            uint16_t used = de->inode ? REC_LEN(de->name_len) : 0;
            if (de->rec_len - used >= need) {
                ext4_dirent_t *ne = de;
                if (used) {
                    ne = (ext4_dirent_t *)(buf + off + used);
                    ne->rec_len = de->rec_len - used;
                    de->rec_len = used;
                }
                ne->inode     = ino;
                ne->name_len  = (uint8_t)len;
                ne->file_type = ft;
                memcpy(ne + 1, name, len);
                r = dir_write_block(dir, pblk, buf);
                kfree(buf);
                return r;
            }
            off += de->rec_len;
        }
    }

    // nowhere had room, a new block on the end
    uint64_t pblk = ext4_alloc_block(fs, last ? last + 1 : 0, false);
    if (!pblk) {
        kfree(buf);
        return -ENOSPC;
    }
    memset(buf, 0, fs->block_size);
    ext4_dirent_t *de = (ext4_dirent_t *)buf;
    de->inode     = ino;
    de->rec_len   = (uint16_t)limit;
    de->name_len  = (uint8_t)len;
    de->file_type = ft;
    memcpy(de + 1, name, len);
    tail_init(fs, buf);
    r = dir_write_block(dir, pblk, buf);
    kfree(buf);

    if (r == 0)
        r = ext4_map_set(dir, n, pblk, 1);
    if (r < 0) {
        ext4_free_blocks(fs, pblk, 1);
        return r;
    }
    ext4_isize_set(&dir->disk, (uint64_t)(n + 1) * fs->block_size);
    return ext4_write_inode(fs, dir->ino, &dir->disk, false);
}

int ext4_dir_remove(ext4_inode_t *dir, ext4_dirpos_t pos) {
    ext4_fs_t *fs   = dir->fs;
    uint64_t   pblk = dir_pblk(dir, pos.lblk);
    if (!pblk)
        return -EIO;

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf)
        return -ENOMEM;
    if (ext4_read_block(fs, pblk, buf) < 0) {
        kfree(buf);
        return -EIO;
    }

    uint32_t off  = 0;
    uint32_t prev = UINT32_MAX;
    int      r    = -ENOENT;
    while (off + sizeof(ext4_dirent_t) <= fs->block_size) {
        ext4_dirent_t *de = (ext4_dirent_t *)(buf + off);
        if (!dirent_ok(fs, de, off))
            break;
        if (off == pos.off) {
            if (prev != UINT32_MAX)
                ((ext4_dirent_t *)(buf + prev))->rec_len += de->rec_len;
            else
                de->inode = 0;
            r = dir_write_block(dir, pblk, buf);
            break;
        }
        prev = off;
        off += de->rec_len;
    }
    kfree(buf);
    return r;
}

// -ENOSPC when the new name doesnt fit where the old one was
int ext4_dir_rename(ext4_inode_t *dir, ext4_dirpos_t pos, const char *name, size_t len) {
    ext4_fs_t *fs = dir->fs;
    if (!len || len > 255)
        return -ENAMETOOLONG;

    // a new name hashes somewhere else in the index
    int r = dir_deindex(dir);
    if (r < 0)
        return r;

    uint64_t pblk = dir_pblk(dir, pos.lblk);
    if (!pblk)
        return -EIO;

    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf)
        return -ENOMEM;
    if (ext4_read_block(fs, pblk, buf) < 0) {
        kfree(buf);
        return -EIO;
    }

    ext4_dirent_t *de = (ext4_dirent_t *)(buf + pos.off);
    if (!dirent_ok(fs, de, pos.off) || REC_LEN(len) > de->rec_len) {
        kfree(buf);
        return -ENOSPC;
    }
    memset(de + 1, 0, de->name_len);
    de->name_len = (uint8_t)len;
    memcpy(de + 1, name, len);
    r = dir_write_block(dir, pblk, buf);
    kfree(buf);
    return r;
}

// first block of a new directory, . and .. in it
int ext4_dir_init(ext4_inode_t *dir, uint32_t parent_ino) {
    ext4_fs_t *fs = dir->fs;

    uint64_t pblk = ext4_alloc_block(fs, 0, false);
    if (!pblk)
        return -ENOSPC;
    uint8_t *buf = kmalloc(fs->block_size);
    if (!buf) {
        ext4_free_blocks(fs, pblk, 1);
        return -ENOMEM;
    }
    memset(buf, 0, fs->block_size);

    ext4_dirent_t *dot = (ext4_dirent_t *)buf;
    dot->inode     = dir->ino;
    dot->rec_len   = REC_LEN(1);
    dot->name_len  = 1;
    dot->file_type = EXT4_FT_DIR;
    memcpy(dot + 1, ".", 1);

    ext4_dirent_t *dotdot = (ext4_dirent_t *)(buf + dot->rec_len);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = (uint16_t)(dir_limit(fs) - dot->rec_len);
    dotdot->name_len  = 2;
    dotdot->file_type = EXT4_FT_DIR;
    memcpy(dotdot + 1, "..", 2);

    tail_init(fs, buf);
    int r = dir_write_block(dir, pblk, buf);
    kfree(buf);

    if (r == 0)
        r = ext4_map_set(dir, 0, pblk, 1);
    if (r < 0) {
        ext4_free_blocks(fs, pblk, 1);
        return r;
    }
    ext4_isize_set(&dir->disk, fs->block_size);
    return ext4_write_inode(fs, dir->ino, &dir->disk, false);
}
