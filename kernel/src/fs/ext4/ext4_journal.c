#include "ext4_priv.h"

#include <mm/kalloc.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <user/errno.h>

// jbd2 replay. we never write the journal ourselves, this only exists so a disk linux didnt
// unmount cleanly can be brought back to what linux last committed and then mounted rw.
// same three passes linux does: scan to the end of the committed log, collect revokes, replay.
// everything in the journal is big endian

#define JBD2_MAGIC              0xC03B3998u
#define JBD2_DESCRIPTOR         1
#define JBD2_COMMIT             2
#define JBD2_SB_V1              3
#define JBD2_SB_V2              4
#define JBD2_REVOKE             5

#define JBD2_FEATURE_COMPAT_CHECKSUM    0x1
#define JBD2_FEATURE_INCOMPAT_REVOKE    0x1
#define JBD2_FEATURE_INCOMPAT_64BIT     0x2
#define JBD2_FEATURE_INCOMPAT_ASYNC     0x4
#define JBD2_FEATURE_INCOMPAT_CSUM_V2   0x8
#define JBD2_FEATURE_INCOMPAT_CSUM_V3   0x10
#define JBD2_FEATURE_INCOMPAT_FAST      0x20
#define JBD2_INCOMPAT_KNOWN (JBD2_FEATURE_INCOMPAT_REVOKE | JBD2_FEATURE_INCOMPAT_64BIT | \
                             JBD2_FEATURE_INCOMPAT_ASYNC | JBD2_FEATURE_INCOMPAT_CSUM_V2 | \
                             JBD2_FEATURE_INCOMPAT_CSUM_V3)

#define JBD2_FLAG_ESCAPE        0x1
#define JBD2_FLAG_SAME_UUID     0x2
#define JBD2_FLAG_LAST_TAG      0x8

// journal superblock offsets
#define JSB_BLOCKSIZE   12
#define JSB_MAXLEN      16
#define JSB_FIRST       20
#define JSB_SEQUENCE    24
#define JSB_START       28
#define JSB_COMPAT      36
#define JSB_INCOMPAT    40
#define JSB_UUID        48
#define JSB_CSUM_TYPE   80
#define JSB_CHECKSUM    0xFC
#define JSB_SIZE        1024

#define JBD2_HDR_SIZE   12
#define JBD2_COMMIT_CSUM 16     // h_chksum[0] in a commit block
#define JBD2_REVOKE_HDR 16      // header + r_count
#define JBD2_TAIL_SIZE  4       // descriptor/revoke block checksum at the very end

enum { PASS_SCAN, PASS_REVOKE, PASS_REPLAY };

typedef struct {
    uint64_t block;
    uint32_t seq;
    bool     used;
} jrevoke_t;

typedef struct {
    ext4_fs_t    *fs;
    ext4_inode_t  inode;      // the journal file, its blocks are found through the normal map code
    uint32_t      bs;
    uint32_t      first;
    uint32_t      maxlen;
    uint32_t      incompat;
    bool          csum;       // v2 or v3, crc32c on everything
    bool          csum_v3;
    uint32_t      seed;
    size_t        tag_bytes;

    uint8_t      *jsb;
    uint8_t      *desc;
    uint8_t      *data;

    jrevoke_t    *revokes;
    size_t        revoke_cap;
    size_t        revoke_count;

    uint32_t      start_seq;
    uint32_t      end_seq;    // first transaction that didnt make it to a commit
    uint32_t      replayed;
    uint32_t      revoked;
    uint32_t      bad_blocks;
    bool          cut_short;  // a checksum ended the log before its real end
} journal_t;

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void be32_set(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// sequence numbers wrap, so "after" is a signed difference like linux tid_gt
static inline bool seq_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

// the replay writes go straight to the device, the fs is read only (RECOVER) while we do this
// and block 0 has the superblock in it on 4K filesystems, which ext4_write_block refuses
static int dev_write_block(ext4_fs_t *fs, uint64_t block, const void *buf) {
    if (block >= fs->blocks_count)
        return -EIO;
    long r = inode_write(fs->dev, buf, fs->block_size, block * fs->block_size);
    return r == (long)fs->block_size ? 0 : -EIO;
}

// logical journal block to where it really is on disk
static int jread(journal_t *j, uint32_t lblk, void *buf) {
    uint64_t pblk;
    uint32_t len;
    bool     unwritten;
    if (ext4_map(&j->inode, lblk, &pblk, &len, &unwritten) < 0 || !pblk || unwritten)
        return -EIO;
    return ext4_read_block(j->fs, pblk, buf);
}

static int jwrite(journal_t *j, uint32_t lblk, const void *buf) {
    uint64_t pblk;
    uint32_t len;
    bool     unwritten;
    if (ext4_map(&j->inode, lblk, &pblk, &len, &unwritten) < 0 || !pblk || unwritten)
        return -EIO;
    return dev_write_block(j->fs, pblk, buf);
}

// the log is a ring between first and maxlen
static inline uint32_t jnext(journal_t *j, uint32_t blk) {
    return ++blk >= j->maxlen ? j->first : blk;
}

static inline uint32_t jskip(journal_t *j, uint32_t blk, uint32_t n) {
    uint32_t ring = j->maxlen - j->first;
    return j->first + (uint32_t)(((uint64_t)(blk - j->first) + n) % ring);
}

// crc over a whole block with its checksum field zeroed, the field is put back after
static bool block_csum_ok(journal_t *j, uint8_t *block, size_t at) {
    if (!j->csum)
        return true;
    uint32_t want = be32(block + at);
    be32_set(block + at, 0);
    uint32_t got = ext4_crc32c(j->seed, block, j->bs);
    be32_set(block + at, want);
    return got == want;
}

static bool data_csum_ok(journal_t *j, const uint8_t *tag, uint32_t seq, const uint8_t *block) {
    if (!j->csum)
        return true;
    uint8_t s[4];
    be32_set(s, seq);
    uint32_t c = ext4_crc32c(j->seed, s, 4);
    c = ext4_crc32c(c, block, j->bs);
    if (j->csum_v3)
        return be32(tag + 12) == c;
    return be16(tag + 4) == (uint16_t)c;
}

static uint32_t tag_flags(journal_t *j, const uint8_t *tag) {
    return j->csum_v3 ? be32(tag + 4) : be16(tag + 6);
}

static uint64_t tag_block(journal_t *j, const uint8_t *tag) {
    uint64_t b = be32(tag);
    if (j->incompat & JBD2_FEATURE_INCOMPAT_64BIT)
        b |= (uint64_t)be32(tag + 8) << 32;
    return b;
}

// how many data blocks a descriptor block is followed by
static uint32_t desc_count(journal_t *j, const uint8_t *desc) {
    size_t   end = j->bs - (j->csum ? JBD2_TAIL_SIZE : 0);
    size_t   off = JBD2_HDR_SIZE;
    uint32_t n   = 0;
    while (off + j->tag_bytes <= end) {
        uint32_t flags = tag_flags(j, desc + off);
        n++;
        off += j->tag_bytes;
        if (!(flags & JBD2_FLAG_SAME_UUID))
            off += 16;
        if (flags & JBD2_FLAG_LAST_TAG)
            break;
    }
    return n;
}

// revoke table, open addressing on the block number. keeps the newest sequence per block
static int revoke_add(journal_t *j, uint64_t block, uint32_t seq) {
    if ((j->revoke_count + 1) * 2 > j->revoke_cap) {
        size_t     cap = j->revoke_cap ? j->revoke_cap * 2 : 256;
        jrevoke_t *t   = kmalloc(cap * sizeof(*t));
        if (!t)
            return -ENOMEM;
        memset(t, 0, cap * sizeof(*t));
        for (size_t i = 0; i < j->revoke_cap; i++) {
            if (!j->revokes[i].used)
                continue;
            size_t h = (size_t)(j->revokes[i].block * 0x9E3779B97F4A7C15ull) & (cap - 1);
            while (t[h].used)
                h = (h + 1) & (cap - 1);
            t[h] = j->revokes[i];
        }
        kfree(j->revokes);
        j->revokes    = t;
        j->revoke_cap = cap;
    }

    size_t h = (size_t)(block * 0x9E3779B97F4A7C15ull) & (j->revoke_cap - 1);
    while (j->revokes[h].used) {
        if (j->revokes[h].block == block) {
            if (seq_after(seq, j->revokes[h].seq))
                j->revokes[h].seq = seq;
            return 0;
        }
        h = (h + 1) & (j->revoke_cap - 1);
    }
    j->revokes[h] = (jrevoke_t){ .block = block, .seq = seq, .used = true };
    j->revoke_count++;
    return 0;
}

// a block logged in transaction seq is skipped if a revoke in seq or later covers it
static bool revoked(journal_t *j, uint64_t block, uint32_t seq) {
    if (!j->revoke_cap)
        return false;
    size_t h = (size_t)(block * 0x9E3779B97F4A7C15ull) & (j->revoke_cap - 1);
    while (j->revokes[h].used) {
        if (j->revokes[h].block == block)
            return !seq_after(seq, j->revokes[h].seq);
        h = (h + 1) & (j->revoke_cap - 1);
    }
    return false;
}

static int scan_revoke_block(journal_t *j, const uint8_t *blk, uint32_t seq) {
    uint32_t count = be32(blk + JBD2_HDR_SIZE);
    size_t   rec   = (j->incompat & JBD2_FEATURE_INCOMPAT_64BIT) ? 8 : 4;
    size_t   end   = j->bs - (j->csum ? JBD2_TAIL_SIZE : 0);
    if (count > end)
        count = (uint32_t)end;

    for (size_t off = JBD2_REVOKE_HDR; off + rec <= count; off += rec) {
        uint64_t b = rec == 8 ? ((uint64_t)be32(blk + off) << 32) | be32(blk + off + 4) : be32(blk + off);
        if (revoke_add(j, b, seq) < 0)
            return -ENOMEM;
    }
    return 0;
}

static int replay_desc(journal_t *j, uint32_t *blk, uint32_t seq) {
    size_t end = j->bs - (j->csum ? JBD2_TAIL_SIZE : 0);
    size_t off = JBD2_HDR_SIZE;

    while (off + j->tag_bytes <= end) {
        const uint8_t *tag   = j->desc + off;
        uint32_t       flags = tag_flags(j, tag);
        uint64_t       home  = tag_block(j, tag);

        *blk = jnext(j, *blk);
        if (jread(j, *blk, j->data) < 0) {
            serial_printf(LOG_ERROR "ext4: journal: couldnt read log block %u\n", *blk);
            return -EIO;
        }

        if (revoked(j, home, seq)) {
            j->revoked++;
        } else if (!data_csum_ok(j, tag, seq, j->data)) {
            // linux skips these too, the block on disk is older but not torn
            serial_printf(LOG_WARN "ext4: journal: bad checksum on block %lu in transaction %u, skipped\n",
                          home, seq);
            j->bad_blocks++;
        } else if (home >= j->fs->blocks_count) {
            serial_printf(LOG_WARN "ext4: journal: block %lu is off the end of the fs, skipped\n", home);
            j->bad_blocks++;
        } else {
            // a block that started with the magic had it zeroed when it was logged
            if (flags & JBD2_FLAG_ESCAPE)
                be32_set(j->data, JBD2_MAGIC);
            if (dev_write_block(j->fs, home, j->data) < 0) {
                serial_printf(LOG_ERROR "ext4: journal: write to block %lu failed\n", home);
                return -EIO;
            }
            j->replayed++;
        }

        off += j->tag_bytes;
        if (!(flags & JBD2_FLAG_SAME_UUID))
            off += 16;
        if (flags & JBD2_FLAG_LAST_TAG)
            break;
    }
    return 0;
}

// walk the log from s_start. scan finds end_seq, the other passes stop there
static int journal_pass(journal_t *j, int pass, uint32_t start) {
    uint32_t blk = start;
    uint32_t seq = j->start_seq;
    // one lap of the ring is the most a valid log can be
    uint64_t steps = 0, ring = j->maxlen - j->first;

    for (;;) {
        if (pass != PASS_SCAN && !seq_after(j->end_seq, seq))
            break;
        if (++steps > ring)
            break;

        if (jread(j, blk, j->desc) < 0) {
            serial_printf(LOG_ERROR "ext4: journal: couldnt read log block %u\n", blk);
            return -EIO;
        }
        if (be32(j->desc) != JBD2_MAGIC || be32(j->desc + 8) != seq)
            break;

        uint32_t type = be32(j->desc + 4);
        if (type == JBD2_DESCRIPTOR) {
            if (pass == PASS_SCAN) {
                if (!block_csum_ok(j, j->desc, j->bs - JBD2_TAIL_SIZE)) {
                    serial_printf(LOG_WARN "ext4: journal: descriptor in transaction %u has a bad checksum, log ends there\n", seq);
                    j->cut_short = true;
                    break;
                }
                uint32_t n = desc_count(j, j->desc);
                steps += n;
                blk = jskip(j, blk, n + 1);
                continue;
            }
            if (pass == PASS_REPLAY) {
                if (replay_desc(j, &blk, seq) < 0)
                    return -EIO;
            } else {
                blk = jskip(j, blk, desc_count(j, j->desc));
            }
            blk = jnext(j, blk);
        } else if (type == JBD2_COMMIT) {
            if (pass == PASS_SCAN && !block_csum_ok(j, j->desc, JBD2_COMMIT_CSUM)) {
                serial_printf(LOG_WARN "ext4: journal: commit of transaction %u has a bad checksum, log ends there\n", seq);
                j->cut_short = true;
                break;
            }
            seq++;
            blk = jnext(j, blk);
        } else if (type == JBD2_REVOKE) {
            if (pass == PASS_SCAN && !block_csum_ok(j, j->desc, j->bs - JBD2_TAIL_SIZE)) {
                serial_printf(LOG_WARN "ext4: journal: revoke block in transaction %u has a bad checksum, log ends there\n", seq);
                j->cut_short = true;
                break;
            }
            if (pass == PASS_REVOKE && scan_revoke_block(j, j->desc, seq) < 0)
                return -ENOMEM;
            blk = jnext(j, blk);
        } else {
            break;
        }
    }

    if (pass == PASS_SCAN)
        j->end_seq = seq;
    return 0;
}

static int journal_open(journal_t *j, ext4_fs_t *fs) {
    ext4_superblock_t *sb = &fs->sb;

    if (!sb->s_journal_inum || sb->s_journal_dev) {
        serial_printf(LOG_WARN "ext4: journal: external journals arent supported\n");
        return -EOPNOTSUPP;
    }

    j->fs        = fs;
    j->bs        = fs->block_size;
    j->inode.fs  = fs;
    j->inode.ino = sb->s_journal_inum;
    if (ext4_read_inode(fs, j->inode.ino, &j->inode.disk) < 0) {
        serial_printf(LOG_ERROR "ext4: journal: couldnt read journal inode %u\n", j->inode.ino);
        return -EIO;
    }

    j->jsb  = kmalloc(j->bs);
    j->desc = kmalloc(j->bs);
    j->data = kmalloc(j->bs);
    if (!j->jsb || !j->desc || !j->data)
        return -ENOMEM;

    if (jread(j, 0, j->jsb) < 0 || be32(j->jsb) != JBD2_MAGIC) {
        serial_printf(LOG_ERROR "ext4: journal: no journal superblock\n");
        return -EIO;
    }

    uint32_t type = be32(j->jsb + 4);
    if (type != JBD2_SB_V1 && type != JBD2_SB_V2) {
        serial_printf(LOG_ERROR "ext4: journal: superblock type %u isnt one we know\n", type);
        return -EIO;
    }

    j->first    = be32(j->jsb + JSB_FIRST);
    j->maxlen   = be32(j->jsb + JSB_MAXLEN);
    j->incompat = type == JBD2_SB_V2 ? be32(j->jsb + JSB_INCOMPAT) : 0;

    uint64_t file_blocks = ext4_isize(&j->inode.disk) / j->bs;
    if (be32(j->jsb + JSB_BLOCKSIZE) != j->bs || j->first == 0 || j->first >= j->maxlen ||
        j->maxlen > file_blocks) {
        serial_printf(LOG_ERROR "ext4: journal: superblock doesnt make sense (bs %u first %u len %u)\n",
                      be32(j->jsb + JSB_BLOCKSIZE), j->first, j->maxlen);
        return -EIO;
    }

    if (j->incompat & JBD2_FEATURE_INCOMPAT_FAST) {
        serial_printf(LOG_WARN "ext4: journal: has fast commits, we cant replay those\n");
        return -EOPNOTSUPP;
    }
    if (j->incompat & ~JBD2_INCOMPAT_KNOWN) {
        serial_printf(LOG_WARN "ext4: journal: unknown features 0x%x\n", j->incompat & ~JBD2_INCOMPAT_KNOWN);
        return -EOPNOTSUPP;
    }

    j->csum_v3 = j->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3;
    j->csum    = j->csum_v3 || (j->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2);
    if (j->csum) {
        if (j->jsb[JSB_CSUM_TYPE] != 4) {   // crc32c
            serial_printf(LOG_WARN "ext4: journal: checksum type %u isnt crc32c\n", j->jsb[JSB_CSUM_TYPE]);
            return -EOPNOTSUPP;
        }
        uint32_t want = be32(j->jsb + JSB_CHECKSUM);
        be32_set(j->jsb + JSB_CHECKSUM, 0);
        uint32_t got = ext4_crc32c(~0u, j->jsb, JSB_SIZE);
        be32_set(j->jsb + JSB_CHECKSUM, want);
        if (got != want) {
            serial_printf(LOG_ERROR "ext4: journal: superblock checksum is wrong\n");
            return -EIO;
        }
        j->seed = ext4_crc32c(~0u, j->jsb + JSB_UUID, 16);
    }

    // same as linux journal_tag_bytes
    if (j->csum_v3) {
        j->tag_bytes = 16;
    } else {
        j->tag_bytes = 12;
        if (j->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2)
            j->tag_bytes += 2;
        if (!(j->incompat & JBD2_FEATURE_INCOMPAT_64BIT))
            j->tag_bytes -= 4;
    }
    return 0;
}

static void journal_close(journal_t *j) {
    kfree(j->jsb);
    kfree(j->desc);
    kfree(j->data);
    kfree(j->revokes);
}

int ext4_journal_replay(ext4_fs_t *fs) {
    journal_t j;
    memset(&j, 0, sizeof(j));

    int r = journal_open(&j, fs);
    if (r < 0) {
        journal_close(&j);
        return r;
    }

    uint32_t start = be32(j.jsb + JSB_START);
    j.start_seq    = be32(j.jsb + JSB_SEQUENCE);

    if (start) {
        if (start < j.first || start >= j.maxlen) {
            serial_printf(LOG_ERROR "ext4: journal: log starts at %u, outside %u..%u\n", start, j.first, j.maxlen);
            journal_close(&j);
            return -EIO;
        }

        if ((r = journal_pass(&j, PASS_SCAN, start)) < 0 ||
            (r = journal_pass(&j, PASS_REVOKE, start)) < 0 ||
            (r = journal_pass(&j, PASS_REPLAY, start)) < 0) {
            serial_printf(LOG_ERROR "ext4: journal: replay failed part way\n");
            journal_close(&j);
            return r;
        }

        serial_printf(LOG_OK "ext4: journal: replayed transactions %u..%u, %u blocks written, %u revoked, %u bad\n",
                      j.start_seq, j.end_seq - 1, j.replayed, j.revoked, j.bad_blocks);
    } else {
        j.end_seq = j.start_seq;
    }
    bool damaged = j.bad_blocks || j.cut_short;

    // empty log. the sequence moves past anything left in it so none of it ever looks valid again
    be32_set(j.jsb + JSB_START, 0);
    be32_set(j.jsb + JSB_SEQUENCE, j.end_seq + 1);
    if (j.csum) {
        be32_set(j.jsb + JSB_CHECKSUM, 0);
        be32_set(j.jsb + JSB_CHECKSUM, ext4_crc32c(~0u, j.jsb, JSB_SIZE));
    }
    if (jwrite(&j, 0, j.jsb) < 0) {
        serial_printf(LOG_ERROR "ext4: journal: couldnt write the journal superblock\n");
        journal_close(&j);
        return -EIO;
    }
    journal_close(&j);

    // the replay may well have rewritten the superblock, so go off the disk copy
    ext4_superblock_t *sb = kmalloc(sizeof(*sb));
    if (!sb)
        return -ENOMEM;
    if (inode_read(fs->dev, sb, sizeof(*sb), EXT4_SUPERBLOCK_OFFSET) != (long)sizeof(*sb) ||
        sb->s_magic != EXT4_SUPER_MAGIC) {
        serial_printf(LOG_ERROR "ext4: journal: superblock is gone after replay\n");
        kfree(sb);
        return -EIO;
    }
    sb->s_feature_incompat &= ~EXT4_INCOMPAT_RECOVER;
    // a damaged log leaves the fs somewhere between two transactions, flag it like linux does
    // so the next e2fsck does a full check
    if (damaged) {
        sb->s_state |= EXT4_STATE_ERROR;
        serial_printf(LOG_WARN "ext4: journal: was damaged, fs marked as having errors, run e2fsck on it\n");
    }
    memcpy(&fs->sb, sb, sizeof(*sb));
    kfree(sb);

    ext4_sb_csum_set(fs);
    long w = inode_write(fs->dev, &fs->sb, sizeof(fs->sb), EXT4_SUPERBLOCK_OFFSET);
    return w == (long)sizeof(fs->sb) ? 0 : -EIO;
}
