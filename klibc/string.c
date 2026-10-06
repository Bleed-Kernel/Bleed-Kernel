#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <cpu/control_registers.h>

#define CR0_TS          (1ULL << 3)

#define SIMD_CHUNK      (64 * 1024)     // most we do with interrupts off in one go

// i only wanna use it for this file for now but ill look at expanding it when i do simd stuff in userspace too
#define MEM_VEC __attribute__((noinline, target("sse2"), \
                               optimize("-ftree-vectorize", "-fvect-cost-model=dynamic", \
                                        "-fno-tree-loop-distribute-patterns")))

typedef struct {
    uint8_t       area[512] __attribute__((aligned(16)));   // fxsave image
    uint64_t      cr0;
    unsigned long flags;
} simd_guard_t;

// dont trap with #NM and saves the registers
static inline void simd_begin(simd_guard_t *g) {
    g->flags = irq_push();

    g->cr0 = read_cr0();
    if (g->cr0 & CR0_TS)
        asm volatile("clts");
    asm volatile("fxsave64 (%0)" :: "r"(g->area) : "memory");
}

static inline void simd_end(simd_guard_t *g) {
    asm volatile("fxrstor64 (%0)" :: "r"(g->area) : "memory");
    if (g->cr0 & CR0_TS)
        write_cr0(g->cr0);
    irq_restore(g->flags);
}

// no restrict on purpose, memmove sends dest below src through here and gcc checks the
// distance itself before it takes the vector loop
MEM_VEC static void copy_fwd_vec(uint8_t *dest, const uint8_t *src, uint64_t n) {
    for (uint64_t i = 0; i < n; i++)
        dest[i] = src[i];
}

// the vectoriser wont take a loop that walks down, so hand it whole 16 byte blocks. each
// one is read in full before its written, thats what makes dest above src safe
MEM_VEC static void copy_back_vec(uint8_t *dest, const uint8_t *src, uint64_t n) {
    while (n >= 16) {
        uint8_t block[16];

        n -= 16;
        __builtin_memcpy(block, src + n, 16);
        __builtin_memcpy(dest + n, block, 16);
    }
    while (n > 0) {
        n--;
        dest[n] = src[n];
    }
}

MEM_VEC static void fill_vec(uint8_t *dest, uint8_t c, uint64_t n) {
    for (uint64_t i = 0; i < n; i++)
        dest[i] = c;
}

/// @brief move memory from destination to source
/// @param dest destination
/// @param src source
/// @param n size to evaluate
/// @return void

void *memmove(void *dest, const void *src, uint64_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (pdest <= psrc || pdest >= psrc + n)
        return memcpy(dest, src, n);

    // overlapping with dest above src, so the chunks come off the end first
    // in a fabulous circular motion
    simd_guard_t guard;

    while (n > 0) {
        uint64_t chunk = n < SIMD_CHUNK ? n : SIMD_CHUNK;

        n -= chunk;
        simd_begin(&guard);
        copy_back_vec(pdest + n, psrc + n, chunk);
        simd_end(&guard);
    }

    return dest;
}

/// @brief compare two blocks of memory
/// @param s1 block 1
/// @param s2 block 2
/// @param n size to evaluate
/// @return result
int memcmp(const void *s1, const void *s2, uint64_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    for (uint64_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}

void *memcpy(void *dest, const void *src, uint64_t n) {
    uint8_t       *pdest = (uint8_t *)dest;
    const uint8_t *psrc  = (const uint8_t *)src;
    simd_guard_t   guard;

    while (n > 0) {
        uint64_t chunk = n < SIMD_CHUNK ? n : SIMD_CHUNK;

        simd_begin(&guard);
        copy_fwd_vec(pdest, psrc, chunk);
        simd_end(&guard);

        pdest += chunk;
        psrc  += chunk;
        n     -= chunk;
    }

    return dest;
}

void *memset(void *s, int c, uint64_t n) {
    uint8_t     *pdest = (uint8_t *)s;
    simd_guard_t guard;

    while (n > 0) {
        uint64_t chunk = n < SIMD_CHUNK ? n : SIMD_CHUNK;

        simd_begin(&guard);
        fill_vec(pdest, (uint8_t)c, chunk);
        simd_end(&guard);

        pdest += chunk;
        n     -= chunk;
    }

    return s;
}

/// @brief gets the length of a string
/// @param string string to evaluate
/// @return uint32 length
size_t strlen(const char *string){
    if (string == NULL) return 0;

    size_t length = 0;
    while (*string != '\0'){
        length++;
        string++;
    }
    return length;
}

#include <stdint.h>

/// @brief copy a string
/// @param dest destination buffer
/// @param src source string
/// @return dest
char *strcpy(char *restrict dest, const char *restrict src) {
    char *d = dest;
    while (*src != '\0') {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}

/// @brief copy at most n chars of src into dest
/// @param dest destination
/// @param src source
/// @param n maximum chars
/// @return dest
char *strncpy(char *restrict dest, const char *restrict src, uint64_t n) {
    char *d = dest;

    for (uint64_t i = 0; i < n; i++) {
        if (src[i] != '\0') {
            d[i] = src[i];
        } else {
            // pad with zeros
            for (; i < n; i++) {
                d[i] = '\0';
            }
            return dest;
        }
    }

    return dest;
}

/// @brief concatenate src onto end of dest
/// @param dest destination string buffer
/// @param src source string to append
/// @return dest
char *strcat(char *restrict dest, const char *restrict src) {
    char *d = dest;

    while (*d != '\0') d++;

    while (*src != '\0') {
        *d++ = *src++;
    }

    *d = '\0';
    return dest;
}

/// @brief compare two strings
/// @param s1 string 1
/// @param s2 string 2
/// @return <0, 0, >0
int strcmp(const char *s1, const char *s2) {
    while (*s1 != '\0' && *s2 != '\0') {
        if (*s1 != *s2) {
            return (*s1 < *s2) ? -1 : 1;
        }
        s1++;
        s2++;
    }

    if (*s1 == *s2) return 0;
    return (*s1 < *s2) ? -1 : 1;
}

/// @brief compare at most n bytes of strings
/// @param s1 string 1
/// @param s2 string 2
/// @param n number of chars to compare
/// @return <0, 0, >0
int strncmp(const char *s1, const char *s2, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return (s1[i] < s2[i]) ? -1 : 1;
        }
        if (s1[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

/// @brief find first occurrence of c in string s
/// @param s string
/// @param c character
/// @return pointer to char or NULL
char *strchr(const char *s, int c) {
    while (*s != '\0') {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return NULL;
}

/// @brief tokenize a string (not thread-safe)
/// @param s input string or NULL to continue
/// @param delim delimiter characters
/// @return next token or NULL
char *strtok(char *restrict s, const char *restrict delim) {
    static char *static_ptr = NULL;
    return strtok_r(s, delim, &static_ptr);
}

/// @brief reentrant tokenizer
/// @param s string to tokenize or NULL
/// @param delim delimiter chars
/// @param save pointer to save state
/// @return next token
char *strtok_r(char *restrict s, const char *restrict delim, char **restrict save) {
    if (s == NULL) {
        s = *save;
    }

    if (s == NULL) {
        return NULL;
    }

    // Skip leading delimiters
    while (*s != '\0') {
        const char *d = delim;
        int found = 0;

        while (*d != '\0') {
            if (*s == *d) {
                found = 1;
                break;
            }
            d++;
        }

        if (!found) break;
        s++;
    }

    if (*s == '\0') {
        *save = NULL;
        return NULL;
    }

    char *token_start = s;

    // Scan until next delimiter
    while (*s != '\0') {
        const char *d = delim;
        while (*d != '\0') {
            if (*s == *d) {
                *s = '\0';
                *save = s + 1;
                return token_start;
            }
            d++;
        }
        s++;
    }

    *save = NULL;
    return token_start;
}

char *strdup(const char *s){
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = kmalloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

size_t strnlen(const char *s, uint64_t n) {
    if (s == NULL) return 0;

    size_t len = 0;
    while (len < n && s[len] != '\0') {
        len++;
    }
    return len;
}