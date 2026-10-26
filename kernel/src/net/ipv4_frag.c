#include <net/ipv4.h>
#include <drivers/serial/serial.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <ansii.h>
#include <string.h>

// bitmap of 8 byte blocks says whats arrived
#define FRAG_BLOCKS     ((IP_MAX_PAYLOAD + 7) / 8)

typedef struct {
    bool     used;
    uint32_t src;
    uint32_t dst;
    uint16_t id;
    uint8_t  proto;
    uint64_t started;
    size_t   total;         // 0 till the last fragment shows up
    size_t   blocks;        // how many bits are set in have
    uint8_t *data;
    uint8_t  have[FRAG_BLOCKS / 8 + 1];
} ip_frag_t;

static ip_frag_t  frags[IP_FRAG_SLOTS];
static spinlock_t frag_lock;
static uint64_t   frags_dropped;

static void frag_free(ip_frag_t *f) {
    kfree(f->data);
    f->data = NULL;
    f->used = false;
}

static ip_frag_t *frag_slot(const ipv4_hdr_t *ip) {
    ip_frag_t *oldest = NULL;
    ip_frag_t *free   = NULL;

    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        ip_frag_t *f = &frags[i];
        if (!f->used) {
            if (!free) free = f;
            continue;
        }
        if (f->src == ip->src && f->dst == ip->dst && f->id == ip->id && f->proto == ip->proto)
            return f;
        if (!oldest || f->started < oldest->started)
            oldest = f;
    }

    if (!free) {
        frags_dropped++;
        frag_free(oldest);
        free = oldest;
    }

    free->data = kmalloc(IP_MAX_PAYLOAD);
    if (!free->data)
        return NULL;
    free->used    = true;
    free->src     = ip->src;
    free->dst     = ip->dst;
    free->id      = ip->id;
    free->proto   = ip->proto;
    free->started = net_now_ms();
    free->total   = 0;
    free->blocks  = 0;
    memset(free->have, 0, sizeof(free->have));
    return free;
}

uint8_t *ipv4_reassemble(const ipv4_hdr_t *ip, const uint8_t *payload, size_t len, size_t *out_len) {
    uint16_t frag   = ntohs(ip->frag);
    size_t   offset = (size_t)(frag & IP_FRAG_MASK) * 8;
    bool     more   = (frag & IP_FLAG_MF) != 0;

    // only the last piece can end off a block boundary
    if (offset + len > IP_MAX_PAYLOAD || (more && (len % 8 || !len)))
        return NULL;

    uint8_t *whole = NULL;

    unsigned long flags = irq_push();
    spinlock_acquire(&frag_lock);

    ip_frag_t *f = frag_slot(ip);
    if (f) {
        memcpy(f->data + offset, payload, len);
        for (size_t b = offset / 8; b < (offset + len + 7) / 8; b++) {
            if (!(f->have[b / 8] & (1u << (b % 8)))) {
                f->have[b / 8] |= (uint8_t)(1u << (b % 8));
                f->blocks++;
            }
        }
        if (!more)
            f->total = offset + len;

        if (f->total && f->blocks == (f->total + 7) / 8) {
            whole    = f->data;
            *out_len = f->total;
            f->data  = NULL;
            f->used  = false;
        }
    }

    spinlock_release(&frag_lock);
    irq_restore(flags);
    return whole;
}

void ipv4_frag_tick(void) {
    uint64_t now     = net_now_ms();
    int      expired = 0;

    unsigned long flags = irq_push();
    spinlock_acquire(&frag_lock);
    for (int i = 0; i < IP_FRAG_SLOTS; i++) {
        if (frags[i].used && now - frags[i].started >= IP_FRAG_TIMEOUT_MS) {
            frag_free(&frags[i]);
            expired++;
        }
    }
    frags_dropped += (uint64_t)expired;
    uint64_t dropped = frags_dropped;
    spinlock_release(&frag_lock);
    irq_restore(flags);

    if (expired)
        serial_printf(LOG_WARN "ipv4: gave up on %d half built packet(s), %llu so far\n", expired, dropped);
}
