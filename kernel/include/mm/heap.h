#pragma once

#include <stddef.h>
#include <stdint.h>

void *heap_allocate(size_t size);
void heap_free(void *ptr);