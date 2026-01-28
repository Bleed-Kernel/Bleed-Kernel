#pragma once

#include <stddef.h>

char *str_copy_from_user(const char *user_ptr, size_t max_len);
int copy_to_user(void *user_dst, const void *kernel_src, size_t len);