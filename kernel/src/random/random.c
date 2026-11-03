#include <random/random.h>
#include <cpu/cpuid.h>
#include <mm/spinlock.h>
#include <ACPI/acpi_hpet.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>

// chacha20 with fast key erasure: every block gives 32 bytes out and the other 32
// become the next key, so reading the key later says nothing about what came before

#define RANDOM_RESEED_MS    1000
#define RANDOM_JITTER_LOOPS 4096

static uint32_t   crng_key[8];
static uint64_t   crng_counter;
static uint32_t   pool[16];         // events land here till the next reseed
static uint32_t   pool_at;
static uint32_t   pool_events;
static uint64_t   last_reseed;
static uint32_t   keyed_secret[8];
static bool       seeded;
static bool       have_hw;
static spinlock_t random_lock;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d)                          \
    a += b; d ^= a; d = rotl32(d, 16);          \
    c += d; b ^= c; b = rotl32(b, 12);          \
    a += b; d ^= a; d = rotl32(d, 8);           \
    c += d; b ^= c; b = rotl32(b, 7)

static void chacha20_block(const uint32_t key[8], uint64_t counter, uint64_t nonce, uint32_t out[16]) {
    uint32_t in[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        (uint32_t)counter, (uint32_t)(counter >> 32), (uint32_t)nonce, (uint32_t)(nonce >> 32),
    };
    uint32_t x[16];
    memcpy(x, in, sizeof(x));

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++)
        out[i] = x[i] + in[i];
}

static bool rdseed64(uint64_t *v) {
    uint8_t ok;
    for (int tries = 0; tries < 32; tries++) {
        asm volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok)
            return true;
        asm volatile("pause");
    }
    return false;
}

static bool rdrand64(uint64_t *v) {
    uint8_t ok;
    for (int tries = 0; tries < 10; tries++) {
        asm volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok)
            return true;
    }
    return false;
}

static void pool_mix_locked(const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        uint32_t w = pool[pool_at] ^ ((uint32_t)p[i] << ((i & 3) * 8));
        pool[pool_at] = rotl32(w, 7) + pool[(pool_at + 7) & 15];
        if ((i & 3) == 3)
            pool_at = (pool_at + 1) & 15;
    }
    pool_events++;
}

// the old key and the whole pool go through one block, its first half is the new key
static void reseed_locked(void) {
    uint32_t mixed[8], out[16];
    for (int i = 0; i < 8; i++)
        mixed[i] = crng_key[i] ^ pool[i] ^ rotl32(pool[i + 8], 13);

    uint64_t extra = rdtsc() ^ ((uint64_t)pool[3] << 32 | pool[11]);
    if (have_hw) {
        uint64_t hw;
        if (rdseed64(&hw) || rdrand64(&hw))
            extra ^= hw;
    }

    chacha20_block(mixed, ++crng_counter, extra, out);
    memcpy(crng_key, out, sizeof(crng_key));
    memset(pool, 0, sizeof(pool));
    memset(out, 0, sizeof(out));
    memset(mixed, 0, sizeof(mixed));
    pool_events = 0;
}

static void jitter_seed(void) {
    static volatile uint8_t scratch[4096];
    uint64_t last = rdtsc();

    for (int i = 0; i < RANDOM_JITTER_LOOPS; i++) {
        scratch[(i * 167) & 4095] += (uint8_t)last;
        uint64_t now   = rdtsc();
        uint32_t delta = (uint32_t)(now - last);
        last = now;
        pool_mix_locked(&delta, sizeof(delta));
    }
}

void random_init(void) {
    uint32_t eax, ebx, ecx, edx;
    bool has_rdrand = cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & (1u << 30));
    bool has_rdseed = cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) && (ebx & (1u << 18));

    unsigned long flags = irq_push();
    spinlock_acquire(&random_lock);

    int hw_words = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t v;
        if ((has_rdseed && rdseed64(&v)) || (has_rdrand && rdrand64(&v))) {
            pool_mix_locked(&v, sizeof(v));
            hw_words++;
        }
    }
    have_hw = hw_words > 0;

    uint64_t clocks[2] = { rdtsc(), hpet_read_counter() };
    pool_mix_locked(clocks, sizeof(clocks));
    jitter_seed();

    reseed_locked();
    seeded      = true;
    last_reseed = rdtsc();

    uint32_t out[16];
    chacha20_block(crng_key, ++crng_counter, 0x5345435245540000ull, out);
    memcpy(keyed_secret, out, sizeof(keyed_secret));
    memset(out, 0, sizeof(out));

    spinlock_release(&random_lock);
    irq_restore(flags);

    serial_printf(LOG_OK "random: chacha20 seeded, %s%d hardware words and timing jitter\n",
                  has_rdseed ? "rdseed, " : has_rdrand ? "rdrand, " : "no rdrand, ", hw_words);
}

bool random_has_hw(void) {
    return have_hw;
}

void random_add(const void *data, size_t len) {
    unsigned long flags = irq_push();
    spinlock_acquire(&random_lock);
    pool_mix_locked(data, len);
    spinlock_release(&random_lock);
    irq_restore(flags);
}

void random_add_timing(uint32_t tag) {
    uint64_t t = rdtsc();
    uint32_t words[2] = { (uint32_t)t ^ tag, (uint32_t)(t >> 32) };
    random_add(words, sizeof(words));
}

static uint64_t reseed_cycles(void) {
    // the tsc rate isnt known here
    return (uint64_t)RANDOM_RESEED_MS * 1000000ull;
}

void random_get_bytes(void *buf, size_t len) {
    uint8_t *p = buf;

    unsigned long flags = irq_push();
    spinlock_acquire(&random_lock);

    if (!seeded) {
        // too early for random_init, the clocks are better than nothing
        uint64_t clocks[2] = { rdtsc(), hpet_read_counter() };
        pool_mix_locked(clocks, sizeof(clocks));
        reseed_locked();
    } else if (pool_events && rdtsc() - last_reseed > reseed_cycles()) {
        reseed_locked();
        last_reseed = rdtsc();
    }

    while (len) {
        uint32_t out[16];
        chacha20_block(crng_key, ++crng_counter, 0, out);
        memcpy(crng_key, out, sizeof(crng_key));

        size_t n = len < 32 ? len : 32;
        memcpy(p, (uint8_t *)out + 32, n);
        memset(out, 0, sizeof(out));
        p   += n;
        len -= n;
    }

    spinlock_release(&random_lock);
    irq_restore(flags);
}

uint32_t random_u32(void) {
    uint32_t v;
    random_get_bytes(&v, sizeof(v));
    return v;
}

uint64_t random_u64(void) {
    uint64_t v;
    random_get_bytes(&v, sizeof(v));
    return v;
}

uint32_t random_keyed32(const uint32_t words[4]) {
    uint32_t out[16];
    chacha20_block(keyed_secret, (uint64_t)words[0] | (uint64_t)words[1] << 32,
                   (uint64_t)words[2] | (uint64_t)words[3] << 32, out);
    uint32_t r = out[0] ^ out[7];
    memset(out, 0, sizeof(out));
    return r;
}
