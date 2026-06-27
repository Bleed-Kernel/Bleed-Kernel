#include <kernel/bootargs.h>
#include <string.h>

// bootargs.c
//
// Parses the Limine boot command line into key/value pairs.
//
// This is designed to run extremely early -- before the kernel's own
// GDT/IDT are loaded and before any memory allocator (kalloc/PMM/VMM)
// is initialized. To make that safe:
//
//   - No heap allocation. Tokens are copied into a static arena buffer
//     living in .bss, which is valid as soon as the kernel image is
//     mapped (i.e. before any C runtime/allocator setup).
//   - No assumption that the cmdline pointer is writable. We only ever
//     read from `cmdline`; '=' splitting happens on our own copies.
//   - No dependency on console/printf. Logging was removed from
//     bootargs_init() since early console state can't be assumed here;
//     log from the caller instead, once it's safe to do so.

#define MAX_BOOTARGS        64
#define BOOTARG_BUFFER_SIZE 2048

static bootarg_t args[MAX_BOOTARGS];
static size_t    arg_count = 0;

static char   arg_buffer[BOOTARG_BUFFER_SIZE];
static size_t buffer_used = 0;

static inline int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

// move this to libc?
static int strcasecmp(const char* s1, const char* s2) {
    if (!s1 || !s2) return -1;
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while (*p1 && (tolower(*p1) == tolower(*p2))) {
        p1++;
        p2++;
    }
    return tolower(*p1) - tolower(*p2);
}

// Copies `len` bytes from src into the static arg arena and NUL-terminates.
// Returns NULL if the arena is exhausted -- caller must stop parsing.
static char* arena_copy(const char* src, size_t len) {
    if (buffer_used + len + 1 > BOOTARG_BUFFER_SIZE) {
        return NULL;
    }
    char* dst = &arg_buffer[buffer_used];
    for (size_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    dst[len] = '\0';
    buffer_used += len + 1;
    return dst;
}

void bootargs_init(const char* cmdline) {
    arg_count   = 0;
    buffer_used = 0;

    if (!cmdline) return;

    const char* ptr = cmdline;

    while (*ptr && arg_count < MAX_BOOTARGS) {
        while (*ptr == ' ') ptr++; // Skip spaces
        if (!*ptr) break;

        const char* start = ptr;
        while (*ptr && *ptr != ' ') ptr++;
        size_t len = ptr - start;

        char* token = arena_copy(start, len);
        if (!token) break; // out of arena space; stop parsing rather than overflow

        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            args[arg_count].key   = token;
            args[arg_count].value = eq + 1;
        } else {
            args[arg_count].key   = token;
            args[arg_count].value = NULL;
        }
        arg_count++;
    }
}

static int bootargs_find(const char* key) {
    if (!key) return -1;
    for (size_t i = 0; i < arg_count; i++) {
        if (strcasecmp(args[i].key, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

const char* bootargs_get(const char* key) {
    int idx = bootargs_find(key);
    if (idx < 0) return NULL;
    return args[idx].value;
}

bool bootargs_has(const char* key) {
    return bootargs_find(key) >= 0;
}

bool bootargs_is(const char* key, const char* expected_value) {
    const char* val = bootargs_get(key);
    if (!val || !expected_value) return false;
    return (strcasecmp(val, expected_value) == 0);
}