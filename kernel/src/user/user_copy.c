#include <stddef.h>
#include <stdint.h>
#include <mm/kalloc.h>

char *str_copy_from_user(const char *user_ptr, size_t max_len) {
    if (!user_ptr) return NULL;
    char *kbuf = kmalloc(max_len);
    if (!kbuf) return NULL;

    for (size_t i = 0; i < max_len - 1; i++) {
        if ((uintptr_t)(user_ptr + i) >= 0x00007ffffffff000ULL) {
            kfree(kbuf, max_len);
            return NULL;
        }
        kbuf[i] = user_ptr[i];
        if (kbuf[i] == 0) break;
    }
    kbuf[max_len - 1] = 0;
    return kbuf;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len) {
    if (!user_dst || !kernel_src) return -1;

    uintptr_t dst = (uintptr_t)user_dst;
    if (dst < 0x0000000000400000ULL || dst + len > 0x00007ffffffff000ULL) return -1;

    const char *src = (const char *)kernel_src;
    char *dst_ptr = (char *)user_dst;

    for (size_t i = 0; i < len; i++) {
        dst_ptr[i] = src[i];
    }

    return 0;
}