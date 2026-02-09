#include <kernel/bootargs.h>
#include <string.h>
#include <stdio.h>
#include <mm/kalloc.h>

#define MAX_BOOTARGS 64

static bootarg_t args[MAX_BOOTARGS];
static size_t arg_count = 0;

static inline int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

// Fixed strcasecmp: ensures the final difference is based on the lowercase version
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

void bootargs_init(const char* cmdline) {
    if (!cmdline) return;

    for(size_t i = 0; i < arg_count; i++) {
        kprintf("Arg: %s = %s\n", args[i].key, args[i].value ? args[i].value : "NULL");
    }

    const char* ptr = cmdline;
    arg_count = 0;

    while (*ptr && arg_count < MAX_BOOTARGS) {
        while (*ptr == ' ') ptr++; // Skip spaces
        if (!*ptr) break;

        const char* start = ptr;
        while (*ptr && *ptr != ' ') ptr++;
        size_t len = ptr - start;

        char* token = kmalloc(len + 1);
        if (!token) return; // Allocation failed
        
        memcpy(token, start, len);
        token[len] = '\0';

        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            args[arg_count].key = token;
            args[arg_count].value = eq + 1;
        } else {
            args[arg_count].key = token;
            args[arg_count].value = NULL;
        }
        arg_count++;
    }
}

const char* bootargs_get(const char* key) {
    for (size_t i = 0; i < arg_count; i++) {
        if (strcasecmp(args[i].key, key) == 0) {
            return args[i].value;
        }
    }
    return NULL;
}

bool bootargs_is(const char* key, const char* expected_value) {
    const char* val = bootargs_get(key);
    if (!val || !expected_value) return false;
    return (strcasecmp(val, expected_value) == 0);
}