#include <stdint.h>
#include <mm/kalloc.h>
#include <stddef.h>

char* utoa_base(uint64_t value, int base, int uppercase) {
    const char* digits = uppercase ?
        "0123456789ABCDEF" : "0123456789abcdef";

    char buffer[65];
    int i = 0;

    if (value == 0) {
        buffer[i++] = '0';
    } else {
        while (value > 0) {
            buffer[i++] = digits[value % base];
            value /= base;
        }
    }

    buffer[i] = '\0';

    char* out = kmalloc(i + 1);
    if (!out) return NULL;

    for (int j = 0; j < i; j++)
        out[j] = buffer[i - 1 - j];

    out[i] = '\0';
    return out;
}

char* itoa_signed(int64_t value) {
    if (value >= 0)
        return utoa_base((uint64_t)value, 10, 0);

    char* tmp = utoa_base((uint64_t)(-value), 10, 0);
    if (!tmp) return NULL;

    size_t len = 1;
    while (tmp[len - 1] != '\0') len++;

    char* out = kmalloc(len + 1);
    if (!out) { kfree(tmp, len + 1); return NULL; }

    out[0] = '-';
    for (size_t i = 0; i < len; i++)
        out[i + 1] = tmp[i];

    kfree(tmp, len + 1);
    return out;
}