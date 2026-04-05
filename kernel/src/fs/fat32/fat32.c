#include <fs/fat32/fat32.h>
#include <fs/vfs.h>
#include <devices/type/blk_device.h>
#include <drivers/ide/ide.h>
#include <mm/kalloc.h>
#include <string.h>
#include <stdio.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <status.h>
#include <stddef.h>
#include <fs/fat32/fat32.h>

const INodeOps_t fat32_dir_ops;
const INodeOps_t fat32_file_ops;

int fat32_read_bytes(fat32_fs_t *fs, uint32_t lba, size_t off,
                             void *buf, size_t count) {
    size_t abs_off = (size_t)lba * fs->bytes_per_sector + off;
    long r = blk_read(fs->blk, buf, count, abs_off);
    return (r < 0) ? -1 : 0;
}

int fat32_write_bytes(fat32_fs_t *fs, uint32_t lba, size_t off,
                              const void *buf, size_t count) {
    size_t abs_off = (size_t)lba * fs->bytes_per_sector + off;
    long r = blk_write(fs->blk, buf, count, abs_off);
    return (r < 0) ? -1 : 0;
}

uint32_t fat32_cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    return fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

uint32_t fat32_read_fat(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = fs->fat_start_lba + fat_offset / fs->bytes_per_sector;
    uint32_t fat_off_in  = fat_offset % fs->bytes_per_sector;

    uint32_t val = 0;
    fat32_read_bytes(fs, fat_sector, fat_off_in, &val, sizeof(val));
    return val & FAT32_CLUSTER_MASK;
}

int fat32_write_fat(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_offset / fs->bytes_per_sector;
    uint32_t fat_off_in = fat_offset % fs->bytes_per_sector;
    value &= FAT32_CLUSTER_MASK;

    for (uint8_t f = 0; f < 2; f++) {
        uint32_t lba = fs->fat_start_lba + f * fs->fat_size_sectors + fat_sector;
        if (fat32_write_bytes(fs, lba, fat_off_in, &value, sizeof(value)) < 0)
            return -1;
    }
    return 0;
}

uint32_t fat32_alloc_cluster(fat32_fs_t *fs, uint32_t prev) {
    for (uint32_t c = 2; c < fs->total_clusters + 2; c++) {
        if (fat32_read_fat(fs, c) == FAT32_CLUSTER_FREE) {
            if (fat32_write_fat(fs, c, 0x0FFFFFFF) < 0)
                return 0;
            if (prev != 0)
                fat32_write_fat(fs, prev, c);

            uint8_t zero[512];
            memset(zero, 0, sizeof(zero));
            uint32_t lba = fat32_cluster_to_lba(fs, c);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++)
                fat32_write_bytes(fs, lba + s, 0, zero, fs->bytes_per_sector);

            return c;
        }
    }
    return 0;
}

void fat32_free_chain(fat32_fs_t *fs, uint32_t start) {
    uint32_t cur = start;
    while (cur >= 2 && cur < FAT32_CLUSTER_EOC) {
        uint32_t next = fat32_read_fat(fs, cur);
        fat32_write_fat(fs, cur, FAT32_CLUSTER_FREE);
        cur = next;
    }
}

// FAT uses their weird naming standard so we can convert it becuase its not the 1990s anymore
bool fat32_name_to_83(const char *name, size_t len, char *out) {
    memset(out, ' ', 11);
    size_t dot = len;

    for (size_t i = 0; i < len; i++)
        if (name[i] == '.') dot = i;

    size_t base_len = (dot == len) ? len : dot;
    size_t ext_len  = (dot == len) ? 0   : (len - dot - 1);

    if (base_len > 8 || ext_len > 3)
        return false;

    for (size_t i = 0; i < base_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    for (size_t i = 0; i < ext_len; i++) {
        char c = name[dot + 1 + i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[8 + i] = c;
    }
    return true;
}

void fat32_83_to_name(const fat32_dirent_t *de, char *out, size_t outsz) {
    char base[9] = {0}, ext[4] = {0};
    int bl = 0, el = 0;

    for (int i = 7; i >= 0; i--)
        if (de->name[i] != ' ') { bl = i + 1; break; }
    for (int i = 2; i >= 0; i--)
        if (de->ext[i]  != ' ') { el = i + 1; break; }

    for (int i = 0; i < bl; i++) {
        char c = de->name[i];
        base[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    for (int i = 0; i < el; i++) {
        char c = de->ext[i];
        ext[i]  = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }

    if (el > 0)
        snprintf(out, outsz, "%s.%s", base, ext);
    else
        snprintf(out, outsz, "%s", base);
}

bool fat32_name_matches(const fat32_dirent_t *de, const char *name, size_t len) {
    char namebuf[13];
    fat32_83_to_name(de, namebuf, sizeof(namebuf));
    size_t bl = strlen(namebuf);
    if (bl != len) return false;
    for (size_t i = 0; i < len; i++) {
        char a = name[i],    b = namebuf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

typedef struct {
    fat32_fs_t *fs;
    uint32_t    cluster;
    uint32_t    offset;
    uint32_t    abs_index;
} fat32_dir_iter_t;

void fat32_iter_init(fat32_dir_iter_t *it, fat32_fs_t *fs, uint32_t first_cluster) {
    it->fs        = fs;
    it->cluster   = first_cluster;
    it->offset    = 0;
    it->abs_index = 0;
}

bool fat32_iter_next_raw(fat32_dir_iter_t *it, fat32_dirent_t *de,
                                 uint32_t *out_cluster, uint32_t *out_off) {
    fat32_fs_t *fs = it->fs;

    while (it->cluster >= 2 && it->cluster < FAT32_CLUSTER_EOC) {
        if (it->offset >= fs->bytes_per_cluster) {
            uint32_t next = fat32_read_fat(fs, it->cluster);
            if (next >= FAT32_CLUSTER_EOC || next < 2) return false;
            it->cluster = next;
            it->offset  = 0;
        }

        uint32_t lba  = fat32_cluster_to_lba(fs, it->cluster);
        uint32_t boff = it->offset;

        if (fat32_read_bytes(fs, lba, boff, de, sizeof(*de)) < 0)
            return false;

        *out_cluster = it->cluster;
        *out_off     = boff;
        it->offset  += sizeof(fat32_dirent_t);

        if ((uint8_t)de->name[0] == FAT32_ENTRY_END) return false;
        return true;
    }
    return false;
}

bool fat32_iter_next(fat32_dir_iter_t *it, fat32_dirent_t *de,
                             uint32_t *out_cluster, uint32_t *out_off) {
    fat32_dirent_t raw;
    uint32_t rc, ro;

    while (fat32_iter_next_raw(it, &raw, &rc, &ro)) {
        if ((uint8_t)raw.name[0] == FAT32_ENTRY_FREE) continue;
        if (raw.attr == FAT32_ATTR_LFN)                continue;
        if (raw.attr & FAT32_ATTR_VOLUME_ID)           continue;
        *de = raw;
        *out_cluster = rc;
        *out_off     = ro;
        return true;
    }
    return false;
}

INode_t *fat32_make_inode(fat32_fs_t *fs, const fat32_dirent_t *de,
                                  uint32_t dirent_cluster, uint32_t dirent_off,
                                  INode_t *parent) {
    bool is_dir = (de->attr & FAT32_ATTR_DIRECTORY) != 0;

    fat32_inode_t *fi = kmalloc(sizeof(*fi));
    if (!fi) return NULL;

    uint32_t first_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
    fi->fs             = fs;
    fi->first_cluster  = first_cluster ? first_cluster : fs->root_cluster;
    fi->file_size      = is_dir ? 0 : de->file_size;
    fi->dirent_cluster = dirent_cluster;
    fi->dirent_offset  = dirent_off;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(fi, sizeof(*fi)); return NULL; }

    memset(inode, 0, sizeof(*inode));
    inode->type          = is_dir ? INODE_DIRECTORY : INODE_FILE;
    inode->ops           = is_dir ? &fat32_dir_ops : &fat32_file_ops;
    inode->internal_data = fi;
    inode->parent        = parent;
    inode->shared        = 1;

    fat32_83_to_name(de, inode->name, sizeof(inode->name));

    return inode;
}

int fat32_lookup(INode_t *dir, const char *name, size_t namelen, INode_t **result) {
    fat32_inode_t *fi = dir->internal_data;
    fat32_fs_t    *fs = fi->fs;

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

    fat32_dir_iter_t it;
    fat32_iter_init(&it, fs, fi->first_cluster);

    fat32_dirent_t de;
    uint32_t dc, doff;
    while (fat32_iter_next(&it, &de, &dc, &doff)) {
        if (fat32_name_matches(&de, name, namelen)) {
            INode_t *inode = fat32_make_inode(fs, &de, dc, doff, dir);
            if (!inode) return status_print_error(OUT_OF_MEMORY);
            *result = inode;
            return 0;
        }
    }
    return -FILE_NOT_FOUND;
}

long fat32_read(INode_t *inode, void *buf, size_t count, size_t offset) {
    fat32_inode_t *fi = inode->internal_data;
    fat32_fs_t    *fs = fi->fs;

    if (offset >= fi->file_size) return 0;
    if (count > fi->file_size - offset) count = fi->file_size - offset;
    if (count == 0) return 0;

    uint32_t cluster_idx = (uint32_t)(offset / fs->bytes_per_cluster);
    uint32_t cluster     = fi->first_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cluster = fat32_read_fat(fs, cluster);
        if (cluster >= FAT32_CLUSTER_EOC || cluster < 2) return 0;
    }

    size_t   read_total  = 0;
    size_t   cluster_off = offset % fs->bytes_per_cluster;

    while (read_total < count && cluster >= 2 && cluster < FAT32_CLUSTER_EOC) {
        uint32_t lba      = fat32_cluster_to_lba(fs, cluster);
        size_t   avail    = fs->bytes_per_cluster - cluster_off;
        size_t   to_read  = count - read_total;
        if (to_read > avail) to_read = avail;

        uint32_t sec_idx = (uint32_t)(cluster_off / fs->bytes_per_sector);
        size_t   sec_off = cluster_off % fs->bytes_per_sector;
        size_t   left    = to_read;
        size_t   done    = 0;

        while (left > 0 && sec_idx < fs->sectors_per_cluster) {
            size_t chunk = fs->bytes_per_sector - sec_off;
            if (chunk > left) chunk = left;
            if (fat32_read_bytes(fs, lba + sec_idx, sec_off,
                                  (uint8_t *)buf + read_total + done, chunk) < 0)
                return read_total > 0 ? (long)read_total : -1;
            done    += chunk;
            left    -= chunk;
            sec_idx++;
            sec_off = 0;
        }

        read_total  += to_read;
        cluster_off  = 0;
        cluster      = fat32_read_fat(fs, cluster);
    }

    return (long)read_total;
}

long fat32_write(INode_t *inode, const void *buf, size_t count, size_t offset) {
    fat32_inode_t *fi = inode->internal_data;
    fat32_fs_t    *fs = fi->fs;

    if (count == 0) return 0;

    uint32_t needed_clusters =
        (uint32_t)((offset + count + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);

    uint32_t cluster    = fi->first_cluster;
    uint32_t prev       = 0;
    uint32_t have       = 0;

    if (cluster < 2) {
        cluster = fat32_alloc_cluster(fs, 0);
        if (!cluster) return status_print_error(OUT_OF_MEMORY);
        fi->first_cluster = cluster;
        fat32_dirent_t de;
        uint32_t lba = fat32_cluster_to_lba(fs, fi->dirent_cluster);
        fat32_read_bytes(fs, lba, fi->dirent_offset % fs->bytes_per_cluster, &de, sizeof(de));
        de.first_cluster_lo = (uint16_t)(cluster & 0xFFFF);
        de.first_cluster_hi = (uint16_t)(cluster >> 16);
        fat32_write_bytes(fs, lba, fi->dirent_offset % fs->bytes_per_cluster, &de, sizeof(de));
        have = 1;
        prev = cluster;
    }

    uint32_t cur = fi->first_cluster;
    have = 0;
    prev = 0;
    while (cur >= 2 && cur < FAT32_CLUSTER_EOC) {
        have++;
        prev = cur;
        cur  = fat32_read_fat(fs, cur);
    }

    while (have < needed_clusters) {
        uint32_t nc = fat32_alloc_cluster(fs, prev);
        if (!nc) return status_print_error(OUT_OF_MEMORY);
        prev = nc;
        have++;
    }

    uint32_t cluster_idx = (uint32_t)(offset / fs->bytes_per_cluster);
    cluster = fi->first_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cluster = fat32_read_fat(fs, cluster);
        if (cluster >= FAT32_CLUSTER_EOC || cluster < 2) return -1;
    }

    size_t written_total = 0;
    size_t cluster_off   = offset % fs->bytes_per_cluster;

    while (written_total < count && cluster >= 2 && cluster < FAT32_CLUSTER_EOC) {
        uint32_t lba    = fat32_cluster_to_lba(fs, cluster);
        size_t   avail  = fs->bytes_per_cluster - cluster_off;
        size_t   to_write = count - written_total;
        if (to_write > avail) to_write = avail;

        uint32_t sec_idx = (uint32_t)(cluster_off / fs->bytes_per_sector);
        size_t   sec_off = cluster_off % fs->bytes_per_sector;
        size_t   left    = to_write;
        size_t   done    = 0;

        while (left > 0 && sec_idx < fs->sectors_per_cluster) {
            size_t chunk = fs->bytes_per_sector - sec_off;
            if (chunk > left) chunk = left;

            if (sec_off != 0 || chunk < fs->bytes_per_sector) {
                uint8_t tmp[512];
                fat32_read_bytes(fs, lba + sec_idx, 0, tmp, fs->bytes_per_sector);
                memcpy(tmp + sec_off, (const uint8_t *)buf + written_total + done, chunk);
                fat32_write_bytes(fs, lba + sec_idx, 0, tmp, fs->bytes_per_sector);
            } else {
                fat32_write_bytes(fs, lba + sec_idx, 0,
                                  (const uint8_t *)buf + written_total + done, chunk);
            }
            done    += chunk;
            left    -= chunk;
            sec_idx++;
            sec_off  = 0;
        }

        written_total += to_write;
        cluster_off    = 0;
        cluster        = fat32_read_fat(fs, cluster);
    }

    size_t new_end = offset + written_total;
    if (new_end > fi->file_size) {
        fi->file_size = (uint32_t)new_end;

        uint32_t dc_lba = fat32_cluster_to_lba(fs, fi->dirent_cluster);
        size_t   doff   = fi->dirent_offset % fs->bytes_per_cluster;
        fat32_dirent_t de;
        fat32_read_bytes(fs, dc_lba, doff, &de, sizeof(de));
        de.file_size = fi->file_size;
        fat32_write_bytes(fs, dc_lba, doff, &de, sizeof(de));
    }

    return (long)written_total;
}

int fat32_truncate(INode_t *inode, size_t new_size) {
    fat32_inode_t *fi = inode->internal_data;
    fat32_fs_t    *fs = fi->fs;

    if (new_size == fi->file_size) return 0;

    if (new_size == 0) {
        fat32_free_chain(fs, fi->first_cluster);
        fi->first_cluster = 0;
        fi->file_size     = 0;
    } else if (new_size < fi->file_size) {
        uint32_t keep = (uint32_t)((new_size + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster);
        uint32_t cur  = fi->first_cluster;
        for (uint32_t i = 1; i < keep && cur < FAT32_CLUSTER_EOC; i++)
            cur = fat32_read_fat(fs, cur);
        if (cur < FAT32_CLUSTER_EOC) {
            uint32_t next = fat32_read_fat(fs, cur);
            fat32_write_fat(fs, cur, 0x0FFFFFFF);
            fat32_free_chain(fs, next);
        }
        fi->file_size = (uint32_t)new_size;
    } else {
        uint8_t z = 0;
        fat32_write(inode, &z, 1, new_size - 1);
        fi->file_size = (uint32_t)new_size;
    }

    uint32_t dc_lba = fat32_cluster_to_lba(fs, fi->dirent_cluster);
    size_t   doff   = fi->dirent_offset % fs->bytes_per_cluster;
    fat32_dirent_t de;
    fat32_read_bytes(fs, dc_lba, doff, &de, sizeof(de));
    de.file_size          = fi->file_size;
    de.first_cluster_lo   = (uint16_t)(fi->first_cluster & 0xFFFF);
    de.first_cluster_hi   = (uint16_t)(fi->first_cluster >> 16);
    fat32_write_bytes(fs, dc_lba, doff, &de, sizeof(de));

    return 0;
}

size_t fat32_size(INode_t *inode) {
    fat32_inode_t *fi = inode->internal_data;
    return fi ? fi->file_size : 0;
}

void fat32_drop(INode_t *inode) {
    if (!inode || !inode->internal_data) return;
    kfree(inode->internal_data, sizeof(fat32_inode_t));
    inode->internal_data = NULL;
}

int fat32_readdir(INode_t *dir, size_t index, INode_t **result) {
    fat32_inode_t *fi = dir->internal_data;
    fat32_fs_t    *fs = fi->fs;

    fat32_dir_iter_t it;
    fat32_iter_init(&it, fs, fi->first_cluster);

    fat32_dirent_t de;
    uint32_t dc, doff;
    size_t visible = 0;

    while (fat32_iter_next(&it, &de, &dc, &doff)) {
        if (de.name[0] == '.' && (de.name[1] == ' ' || de.name[1] == '.'))
            continue;
        if (visible == index) {
            INode_t *inode = fat32_make_inode(fs, &de, dc, doff, dir);
            if (!inode) return status_print_error(OUT_OF_MEMORY);
            inode->shared++;
            *result = inode;
            return 0;
        }
        visible++;
    }
    return -FILE_NOT_FOUND;
}

int fat32_create(INode_t *parent, const char *name, size_t namelen,
                         INode_t **result, inode_type node_type) {
    fat32_inode_t *pfi = parent->internal_data;
    fat32_fs_t    *fs  = pfi->fs;

    char name83[16];
    if (!fat32_name_to_83(name, namelen, name83))
        return status_print_error(NAME_LIMITS);

    uint32_t slot_cluster = 0, slot_off = 0;
    uint32_t prev_cluster = 0;

    uint32_t cluster = pfi->first_cluster;
    bool     found   = false;

    while (cluster >= 2 && cluster < FAT32_CLUSTER_EOC && !found) {
        uint32_t lba = fat32_cluster_to_lba(fs, cluster);
        for (uint32_t off = 0; off < fs->bytes_per_cluster; off += sizeof(fat32_dirent_t)) {
            fat32_dirent_t de;
            fat32_read_bytes(fs, lba, off, &de, sizeof(de));
            uint8_t first = (uint8_t)de.name[0];
            if (first == FAT32_ENTRY_FREE || first == FAT32_ENTRY_END) {
                slot_cluster = cluster;
                slot_off     = off;
                found        = true;
                break;
            }
        }
        prev_cluster = cluster;
        cluster      = fat32_read_fat(fs, cluster);
    }

    if (!found) {
        uint32_t nc = fat32_alloc_cluster(fs, prev_cluster);
        if (!nc) return status_print_error(OUT_OF_MEMORY);
        slot_cluster = nc;
        slot_off     = 0;
    }

    uint32_t new_cluster = fat32_alloc_cluster(fs, 0);
    if (!new_cluster) return status_print_error(OUT_OF_MEMORY);

    fat32_dirent_t de;
    memset(&de, 0, sizeof(de));
    memcpy(de.name, name83, 8);
    memcpy(de.ext,  name83 + 8, 3);
    de.attr             = (node_type == INODE_DIRECTORY) ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
    de.first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
    de.first_cluster_hi = (uint16_t)(new_cluster >> 16);
    de.file_size        = 0;

    uint32_t slot_lba = fat32_cluster_to_lba(fs, slot_cluster);
    fat32_write_bytes(fs, slot_lba, slot_off, &de, sizeof(de));

    if (node_type == INODE_DIRECTORY) {
        fat32_dirent_t dot, dotdot;
        memset(&dot, 0, sizeof(dot));
        memset(&dotdot, 0, sizeof(dotdot));

        memset(dot.name, ' ', 8); memset(dot.ext, ' ', 3);
        dot.name[0] = '.';
        dot.attr = FAT32_ATTR_DIRECTORY;
        dot.first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);
        dot.first_cluster_hi = (uint16_t)(new_cluster >> 16);

        memset(dotdot.name, ' ', 8); memset(dotdot.ext, ' ', 3);
        dotdot.name[0] = '.'; dotdot.name[1] = '.';
        dotdot.attr = FAT32_ATTR_DIRECTORY;
        dotdot.first_cluster_lo = (uint16_t)(pfi->first_cluster & 0xFFFF);
        dotdot.first_cluster_hi = (uint16_t)(pfi->first_cluster >> 16);

        uint32_t new_lba = fat32_cluster_to_lba(fs, new_cluster);
        fat32_write_bytes(fs, new_lba, 0,                      &dot,    sizeof(dot));
        fat32_write_bytes(fs, new_lba, sizeof(fat32_dirent_t), &dotdot, sizeof(dotdot));
    }

    fat32_inode_t *fi = kmalloc(sizeof(*fi));
    if (!fi) return status_print_error(OUT_OF_MEMORY);
    fi->fs             = fs;
    fi->first_cluster  = new_cluster;
    fi->file_size      = 0;
    fi->dirent_cluster = slot_cluster;
    fi->dirent_offset  = slot_off;

    INode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) { kfree(fi, sizeof(*fi)); return status_print_error(OUT_OF_MEMORY); }
    memset(inode, 0, sizeof(*inode));
    inode->type          = (node_type == INODE_DIRECTORY) ? INODE_DIRECTORY : INODE_FILE;
    inode->ops           = (node_type == INODE_DIRECTORY) ? &fat32_dir_ops : &fat32_file_ops;
    inode->internal_data = fi;
    inode->parent        = parent;
    inode->shared        = 1;

    *result = inode;
    return 0;
}

int fat32_unlink(INode_t *dir, const char *name, size_t namelen) {
    fat32_inode_t *fi = dir->internal_data;
    fat32_fs_t    *fs = fi->fs;

    fat32_dir_iter_t it;
    fat32_iter_init(&it, fs, fi->first_cluster);

    fat32_dirent_t de;
    uint32_t dc, doff;

    while (fat32_iter_next(&it, &de, &dc, &doff)) {
        if (!fat32_name_matches(&de, name, namelen)) continue;

        if (de.attr & FAT32_ATTR_DIRECTORY) {
            uint32_t fc = ((uint32_t)de.first_cluster_hi << 16) | de.first_cluster_lo;
            fat32_dir_iter_t ci;
            fat32_iter_init(&ci, fs, fc);
            fat32_dirent_t cde; uint32_t cc, co;
            bool has_children = false;
            while (fat32_iter_next(&ci, &cde, &cc, &co)) {
                if (cde.name[0] == '.' && (cde.name[1] == ' ' || cde.name[1] == '.'))
                    continue;
                has_children = true;
                break;
            }
            if (has_children) return status_print_error(OUT_OF_BOUNDS);
        }

        uint32_t fc = ((uint32_t)de.first_cluster_hi << 16) | de.first_cluster_lo;
        if (fc >= 2) fat32_free_chain(fs, fc);

        de.name[0] = (char)FAT32_ENTRY_FREE;
        uint32_t lba = fat32_cluster_to_lba(fs, dc);
        fat32_write_bytes(fs, lba, doff, &de, sizeof(de));

        return 0;
    }
    return -FILE_NOT_FOUND;
}

int fat32_rename(INode_t *dir, const char *oldname, size_t oldlen,
                         const char *newname, size_t newlen) {
    fat32_inode_t *fi = dir->internal_data;
    fat32_fs_t    *fs = fi->fs;

    // the compiler bitches and moans if its not aligned
    char new83[16];
    if (!fat32_name_to_83(newname, newlen, new83))
        return status_print_error(NAME_LIMITS);

    fat32_dir_iter_t it;
    fat32_iter_init(&it, fs, fi->first_cluster);

    fat32_dirent_t de;
    uint32_t dc, doff;

    while (fat32_iter_next(&it, &de, &dc, &doff)) {
        if (!fat32_name_matches(&de, oldname, oldlen)) continue;

        memcpy(de.name, new83,     8);
        memcpy(de.ext,  new83 + 8, 3);

        uint32_t lba = fat32_cluster_to_lba(fs, dc);
        fat32_write_bytes(fs, lba, doff, &de, sizeof(de));
        return 0;
    }
    return -FILE_NOT_FOUND;
}

const INodeOps_t fat32_dir_ops = {
    .lookup  = fat32_lookup,
    .readdir = fat32_readdir,
    .create  = fat32_create,
    .unlink  = fat32_unlink,
    .rename  = fat32_rename,
    .drop    = fat32_drop,
};

const INodeOps_t fat32_file_ops = {
    .read     = fat32_read,
    .write    = fat32_write,
    .truncate = fat32_truncate,
    .size     = fat32_size,
    .drop     = fat32_drop,
};

int fat32_mount(blk_device_t *blk, INode_t **root) {
    if (!blk || !blk->drive) {
        serial_printf(LOG_ERROR "fat32: null blk_device or drive pointer\n");
        return -1;
    }
    if (!blk->drive->present) {
        serial_printf(LOG_ERROR "fat32: drive not present\n");
        return -1;
    }
    if (blk->sector_count == 0) {
        serial_printf(LOG_ERROR "fat32: partition has zero sectors\n");
        return -1;
    }

    uint8_t raw_sector[512];
    if (ide_read_sectors(blk->drive, blk->lba_start, 1, raw_sector) < 0) {
        serial_printf(LOG_ERROR "fat32: failed to read sector %u from drive\n", blk->lba_start);
        return -1;
    }

    fat32_bpb_t bpb;
    memcpy(&bpb, raw_sector, sizeof(bpb));

    // Dump key BPB fields incase we have a bad image
    // maybe one day we can have some sort of error reporting kernel side too?
    serial_printf(LOG_INFO "fat32: BPB bytes_per_sector=%u secs_per_cluster=%u "
                  "reserved=%u num_fats=%u fat_size32=%u root_cluster=%u "
                  "root_entry_count=%u total32=%u\n",
                  bpb.bytes_per_sector, bpb.sectors_per_cluster,
                  bpb.reserved_sector_count, bpb.num_fats,
                  bpb.fat_size_32, bpb.root_cluster,
                  bpb.root_entry_count, bpb.total_sectors_32);

    // Validate the boot sector signature
    if (raw_sector[510] != 0x55 || raw_sector[511] != 0xAA) {
        serial_printf(LOG_ERROR "fat32: missing boot signature (got %02x %02x)\n",
                      raw_sector[510], raw_sector[511]);
        return -1;
    }

    if (bpb.bytes_per_sector == 0 || bpb.sectors_per_cluster == 0 ||
        bpb.num_fats == 0 || bpb.fat_size_32 == 0) {
        serial_printf(LOG_ERROR "fat32: invalid BPB fields\n");
        return -1;
    }
    if (bpb.root_entry_count != 0) {
        serial_printf(LOG_ERROR "fat32: not FAT32 (root_entry_count=%u, expected 0)\n",
                      bpb.root_entry_count);
        return -1;
    }

    fat32_fs_t *fs = kmalloc(sizeof(*fs));
    if (!fs) return -1;
    memset(fs, 0, sizeof(*fs));

    fs->blk                 = blk;
    fs->bytes_per_sector    = bpb.bytes_per_sector;
    fs->sectors_per_cluster = bpb.sectors_per_cluster;
    fs->bytes_per_cluster   = bpb.bytes_per_sector * bpb.sectors_per_cluster;
    fs->fat_start_lba       = bpb.reserved_sector_count;
    fs->fat_size_sectors    = bpb.fat_size_32;
    fs->data_start_lba      = bpb.reserved_sector_count
                              + bpb.num_fats * bpb.fat_size_32;
    fs->root_cluster        = bpb.root_cluster;

    uint32_t total_sectors = bpb.total_sectors_32 ? bpb.total_sectors_32
                                                   : bpb.total_sectors_16;
    if (total_sectors <= fs->data_start_lba) {
        serial_printf(LOG_ERROR "fat32: data_start_lba (%u) >= total_sectors (%u)\n",
                      fs->data_start_lba, total_sectors);
        kfree(fs, sizeof(*fs));
        return -1;
    }
    uint32_t data_sectors  = total_sectors - fs->data_start_lba;
    fs->total_clusters     = data_sectors / bpb.sectors_per_cluster;

    fat32_dirent_t root_de;
    memset(&root_de, 0, sizeof(root_de));
    memset(root_de.name, ' ', 8);
    memset(root_de.ext,  ' ', 3);
    root_de.attr             = FAT32_ATTR_DIRECTORY;
    root_de.first_cluster_lo = (uint16_t)(fs->root_cluster & 0xFFFF);
    root_de.first_cluster_hi = (uint16_t)(fs->root_cluster >> 16);

    INode_t *root_inode = fat32_make_inode(fs, &root_de, 0, 0, NULL);
    if (!root_inode) {
        kfree(fs, sizeof(*fs));
        return -1;
    }

    *root = root_inode;
    return 0;
}