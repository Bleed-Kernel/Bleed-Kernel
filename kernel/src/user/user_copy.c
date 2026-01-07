#include <stddef.h>
#include <stdint.h>
#include <mm/kalloc.h>

char *user_copy_string(const char *user_ptr, size_t max_len) {
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