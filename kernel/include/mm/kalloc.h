#pragma once
#include <stddef.h>

/// @brief allocate BYTES (rounded up to the nearist 4kib)
/// @param bytes bytes (will be rounded to nearist 4kib)
/// @return allocated virtual address
void* kmalloc(size_t bytes);

/// @brief free memory at address of size bytes (to the nearist upper 4kib)
/// @param addr address
/// @param bytes size to free
void kfree(void* addr, size_t bytes);