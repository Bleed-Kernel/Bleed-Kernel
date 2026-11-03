#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define GRND_NONBLOCK   0x1     // linux numbers, we never block anyway
#define GRND_RANDOM     0x2
#define GRND_INSECURE   0x4

// seed off rdseed/rdrand, the clocks and a burst of timing jitter. call once early
void random_init(void);
bool random_has_hw(void);

// fill buf from the chacha20 generator, never blocks anyway
void random_get_bytes(void *buf, size_t len);
uint32_t random_u32(void);
uint64_t random_u64(void);

// stir something unpredictable in, nothing gets credited for it
void random_add(const void *data, size_t len);

// an event happened now, the cycle counter is the useful part
void random_add_timing(uint32_t tag);

// a keyed hash under a boot secret, for things like tcp sequence numbers
uint32_t random_keyed32(const uint32_t words[4]);

// /dev/random and /dev/urandom
void random_device_init(void);
