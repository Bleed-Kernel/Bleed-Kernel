#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALIGN_UP(size, align) (((size) + ((align) - 1)) & ~((align) - 1))

void *heap_allocate(size_t size);
void heap_free(void *ptr);