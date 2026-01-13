#pragma once

#include <stddef.h>

char *from_user_copy_string(const char *user_ptr, size_t max_len);
int from_kernel_copy_user(void *user_dst, const void *kernel_src, size_t len);