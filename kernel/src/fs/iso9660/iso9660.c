/*
    read only iso9660 with rock ridge names, mostly so the release iso can carry data.img.
    files are one run of blocks so reading one is just an offset into the device
*/

#include <fs/iso9660/iso9660.h>
#include <fs/vfs.h>
#include <ACPI/acpi_time.h>
#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <user/errno.h>

typedef struct {
    INode_t *dev;
    uint32_t blocks;
    uint8_t  susp_skip;     // bytes before the rock ridge entries in every record
    bool     rock_ridge;
    char     label[33];
} iso_fs_t;

typedef struct {
    iso_fs_t *fs;
    uint32_t  extent;
    uint32_t  size;
    int64_t   mtime;
} iso_inode_t;

typedef struct {
    iso_fs_t *fs;
    uint32_t  extent;
    uint32_t  size;
    uint32_t  pos;
    uint32_t  loaded;       // which block of the directory is in buf
    uint8_t  *buf;
} iso_dir_iter_t;

static const INodeOps_t iso_dir_ops;
static const INodeOps_t iso_file_ops;

static int iso_read_dev(iso_fs_t *fs, uint64_t off, void *buf, size_t count) {
    return inode_read(fs->dev, buf, count, off) == (long)count ? 0 : -EIO;
}

static int64_t iso_time(const uint8_t *d) {
    return clock_from_civil(1900 + d[0], d[1], d[2], d[3], d[4], d[5]);
}

static char iso_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// directory walk

static bool iso_iter_init(iso_dir_iter_t *it, iso_fs_t *fs, uint32_t extent, uint32_t size) {
    it->fs     = fs;
    it->extent = extent;
    it->size   = size;
    it->pos    = 0;
    it->loaded = UINT32_MAX;
    it->buf    = kmalloc(ISO_SECTOR);
    return it->buf != NULL;
}

static void iso_iter_done(iso_dir_iter_t *it) {
    kfree(it->buf);
    it->buf = NULL;
}

// next record, "." and ".." included. the pointer is only good until the next call
static iso_dirent_t *iso_iter_raw(iso_dir_iter_t *it) {
    while (it->pos < it->size) {
        uint32_t block = it->pos / ISO_SECTOR;
        uint32_t off   = it->pos % ISO_SECTOR;

        if (block != it->loaded) {
            if (iso_read_dev(it->fs, (uint64_t)(it->extent + block) * ISO_SECTOR, it->buf, ISO_SECTOR) < 0) {
                serial_printf(LOG_ERROR "iso9660: couldnt read directory block %u\n", it->extent + block);
                return NULL;
            }
            it->loaded = block;
        }

        iso_dirent_t *d = (iso_dirent_t *)(it->buf + off);

        // records never cross a block, a zero length means the rest of this one is padding
        if (off + sizeof(iso_dirent_t) > ISO_SECTOR || d->length == 0) {
            it->pos = (block + 1) * ISO_SECTOR;
            continue;
        }
        if (d->length < sizeof(iso_dirent_t) + d->name_len || off + d->length > ISO_SECTOR) {
            serial_printf(LOG_WARN "iso9660: bad record in block %u (len=%u name=%u)\n",
                          it->extent + block, d->length, d->name_len);
            return NULL;
        }

        it->pos += d->length;
        return d;
    }
    return NULL;
}

static iso_dirent_t *iso_iter_next(iso_dir_iter_t *it) {
    iso_dirent_t *d;
    while ((d = iso_iter_raw(it))) {
        if (d->name_len == 1 && (d->name[0] == 0 || d->name[0] == 1))
            continue;
        return d;
    }
    return NULL;
}

// names

static uint8_t *iso_susp(const iso_fs_t *fs, const iso_dirent_t *d, size_t *len) {
    size_t start = sizeof(iso_dirent_t) + d->name_len + !(d->name_len & 1) + fs->susp_skip;
    if (start >= d->length) {
        *len = 0;
        return NULL;
    }
    *len = d->length - start;
    return (uint8_t *)d + start;
}

// the rock ridge NM name, false if the record hasnt got one
static bool iso_rr_name(const iso_fs_t *fs, const iso_dirent_t *d, char *out, size_t size) {
    size_t   len;
    uint8_t *su  = iso_susp(fs, d, &len);
    size_t   pos = 0, n = 0;
    bool     found = false;

    while (su && pos + 4 <= len) {
        uint8_t elen = su[pos + 2];
        if (elen < 4 || pos + elen > len)
            break;
        if (su[pos] == 'S' && su[pos + 1] == 'T')
            break;

        if (su[pos] == 'N' && su[pos + 1] == 'M' && elen >= 5) {
            uint8_t flags = su[pos + 4];
            if (flags & 0x06)
                return false;   // "." or "..", the caller skips those anyway
            for (size_t i = 5; i < elen && n + 1 < size; i++)
                out[n++] = (char)su[pos + i];
            found = true;
            if (!(flags & 0x01))
                break;
        }
        pos += elen;
    }
    if (found)
        out[n] = '\0';
    return found;
}

// plain iso names are "NAME.EXT;1" in caps, show them how linux does: lowercase, no version
static void iso_plain_name(const iso_dirent_t *d, char *out, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < d->name_len && d->name[i] != ';' && n + 1 < size; i++)
        out[n++] = iso_lower(d->name[i]);
    if (n > 0 && out[n - 1] == '.')
        n--;
    out[n] = '\0';
}

static void iso_name(const iso_fs_t *fs, const iso_dirent_t *d, char *out, size_t size) {
    if (!fs->rock_ridge || !iso_rr_name(fs, d, out, size))
        iso_plain_name(d, out, size);
}

static bool iso_name_matches(const iso_fs_t *fs, const char *entry, const char *name, size_t namelen) {
    if (strlen(entry) != namelen)
        return false;
    if (fs->rock_ridge)
        return memcmp(entry, name, namelen) == 0;
    for (size_t i = 0; i < namelen; i++) {
        if (iso_lower(entry[i]) != iso_lower(name[i]))
            return false;
    }
    return true;
}

// inodes

static INode_t *iso_make_inode(iso_fs_t *fs, const iso_dirent_t *d, const char *name, INode_t *parent) {
    bool is_dir = (d->flags & ISO_FLAG_DIR) != 0;

    if (d->flags & ISO_FLAG_MULTI)
        serial_printf(LOG_WARN "iso9660: %s is split over extents, only the first one is readable\n", name);

    iso_inode_t *fi = kmalloc(sizeof(*fi));
    if (!fi) return NULL;
    fi->fs     = fs;
    fi->extent = d->extent + d->ext_attr_length;
    fi->size   = d->size;
    fi->mtime  = iso_time(d->date);

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(fi); return NULL; }

    memset(inode, 0, sizeof(*inode));
    inode->type          = is_dir ? INODE_DIRECTORY : INODE_FILE;
    inode->ops           = is_dir ? &iso_dir_ops : &iso_file_ops;
    inode->internal_data = fi;
    inode->parent        = parent;
    inode->shared        = 1;
    strncpy(inode->name, name, sizeof(inode->name) - 1);
    return inode;
}

static int iso_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result) {
    iso_inode_t *fi = dir->internal_data;

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

    iso_dir_iter_t it;
    if (!iso_iter_init(&it, fi->fs, fi->extent, fi->size))
        return -ENOMEM;

    char          entry[256];
    iso_dirent_t *d;
    int           r = -ENOENT;
    while ((d = iso_iter_next(&it))) {
        iso_name(fi->fs, d, entry, sizeof(entry));
        if (iso_name_matches(fi->fs, entry, name, namelen)) {
            *result = iso_make_inode(fi->fs, d, entry, dir);
            r = *result ? 0 : -ENOMEM;
            break;
        }
    }
    iso_iter_done(&it);
    return r;
}

static int iso_readdir(INode_t *dir, size_t index, INode_t **result) {
    iso_inode_t *fi = dir->internal_data;

    iso_dir_iter_t it;
    if (!iso_iter_init(&it, fi->fs, fi->extent, fi->size))
        return -ENOMEM;

    char          entry[256];
    iso_dirent_t *d;
    size_t        i = 0;
    int           r = -ENOENT;
    while ((d = iso_iter_next(&it))) {
        if (i++ != index)
            continue;
        iso_name(fi->fs, d, entry, sizeof(entry));
        *result = iso_make_inode(fi->fs, d, entry, dir);
        r = *result ? 0 : -ENOMEM;
        break;
    }
    iso_iter_done(&it);
    return r;
}

static long iso_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    iso_inode_t *fi = inode->internal_data;

    if (offset >= fi->size) return 0;
    if (count > fi->size - offset) count = fi->size - offset;
    if (count == 0) return 0;

    return inode_read(fi->fs->dev, buf, count, (uint64_t)fi->extent * ISO_SECTOR + offset);
}

static size_t iso_size(INode_t *inode) {
    iso_inode_t *fi = inode->internal_data;
    return fi ? fi->size : 0;
}

static void iso_times(INode_t *inode, int64_t *atime, int64_t *mtime, int64_t *ctime) {
    iso_inode_t *fi = inode->internal_data;
    if (!fi) return;
    *atime = *mtime = *ctime = fi->mtime;
}

static int iso_statfs(INode_t *root, uint64_t *block_size, uint64_t *total, uint64_t *free) {
    iso_inode_t *fi = root->internal_data;
    if (!fi) return -EINVAL;
    *block_size = ISO_SECTOR;
    *total      = fi->fs->blocks;
    *free       = 0;
    return 0;
}

static int iso_label(INode_t *root, char *out, size_t size) {
    iso_inode_t *fi = root->internal_data;
    if (!fi || size == 0) return -EINVAL;
    strncpy(out, fi->fs->label, size - 1);
    out[size - 1] = '\0';
    return out[0] ? 0 : -ENOENT;
}

static void iso_drop(INode_t *inode) {
    if (!inode || !inode->internal_data) return;
    kfree(inode->internal_data);
    inode->internal_data = NULL;
}

static const INodeOps_t iso_dir_ops = {
    .lookup  = iso_lookup,
    .readdir = iso_readdir,
    .drop    = iso_drop,
    .times   = iso_times,
    .statfs  = iso_statfs,
    .label   = iso_label,
};

static const INodeOps_t iso_file_ops = {
    .read  = iso_read,
    .size  = iso_size,
    .drop  = iso_drop,
    .times = iso_times,
};

// mount

bool iso9660_probe(INode_t *dev_inode) {
    char id[5];
    return inode_read(dev_inode, id, sizeof(id), ISO_MAGIC_OFF) == (long)sizeof(id) &&
           memcmp(id, ISO_MAGIC, sizeof(id)) == 0;
}

// rock ridge puts an SP entry at the start of the roots "." record
static void iso_detect_rock_ridge(iso_fs_t *fs, uint32_t extent, uint32_t size) {
    iso_dir_iter_t it;
    if (!iso_iter_init(&it, fs, extent, size))
        return;

    iso_dirent_t *d = iso_iter_raw(&it);
    size_t        len;
    uint8_t      *su = d ? iso_susp(fs, d, &len) : NULL;
    if (su && len >= 7 && su[0] == 'S' && su[1] == 'P' && su[4] == 0xBE && su[5] == 0xEF) {
        fs->rock_ridge = true;
        fs->susp_skip  = su[6];
    }
    iso_iter_done(&it);
}

int iso9660_mount(INode_t *dev_inode, INode_t **root) {
    iso_pvd_t *pvd = kmalloc(ISO_SECTOR);
    if (!pvd) return -ENOMEM;

    bool found = false;
    for (uint32_t i = 0; i < ISO_VD_MAX && !found; i++) {
        if (inode_read(dev_inode, pvd, ISO_SECTOR, (uint64_t)(ISO_VD_LBA + i) * ISO_SECTOR) != ISO_SECTOR) {
            serial_printf(LOG_ERROR "iso9660: couldnt read volume descriptor %u\n", ISO_VD_LBA + i);
            break;
        }
        if (memcmp(pvd->id, ISO_MAGIC, 5) != 0 || pvd->type == ISO_VD_END)
            break;
        found = pvd->type == ISO_VD_PRIMARY;
    }
    if (!found) {
        serial_printf(LOG_ERROR "iso9660: no primary volume descriptor\n");
        kfree(pvd);
        return -EIO;
    }
    if (pvd->block_size != ISO_SECTOR) {
        serial_printf(LOG_ERROR "iso9660: block size %u, only %u is supported\n", pvd->block_size, ISO_SECTOR);
        kfree(pvd);
        return -EIO;
    }

    iso_fs_t *fs = kmalloc(sizeof(*fs));
    if (!fs) { kfree(pvd); return -ENOMEM; }
    memset(fs, 0, sizeof(*fs));
    fs->dev    = dev_inode;
    fs->blocks = pvd->space;

    memcpy(fs->label, pvd->volume_id, sizeof(pvd->volume_id));
    for (int i = sizeof(pvd->volume_id) - 1; i >= 0 && (fs->label[i] == ' ' || fs->label[i] == '\0'); i--)
        fs->label[i] = '\0';

    iso_dirent_t *rd     = (iso_dirent_t *)pvd->root;
    uint32_t      extent = rd->extent + rd->ext_attr_length;
    uint32_t      size   = rd->size;
    kfree(pvd);

    iso_detect_rock_ridge(fs, extent, size);

    iso_inode_t *root_fi = kmalloc(sizeof(*root_fi));
    if (!root_fi) { kfree(fs); return -ENOMEM; }
    root_fi->fs     = fs;
    root_fi->extent = extent;
    root_fi->size   = size;
    root_fi->mtime  = 0;

    INode_t *root_inode = kmalloc(sizeof(*root_inode));
    if (!root_inode) { kfree(root_fi); kfree(fs); return -ENOMEM; }
    memset(root_inode, 0, sizeof(*root_inode));
    root_inode->type          = INODE_DIRECTORY;
    root_inode->ops           = &iso_dir_ops;
    root_inode->internal_data = root_fi;
    root_inode->shared        = 1;

    serial_printf(LOG_OK "iso9660: mounted \"%s\" - %u blocks, root at %u (%u bytes), rock ridge %s\n",
                  fs->label, fs->blocks, extent, size, fs->rock_ridge ? "yes" : "no");

    *root = root_inode;
    return 0;
}
