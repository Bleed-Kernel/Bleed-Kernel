#pragma once

#include <stdint.h>
#include <stddef.h>

static inline void framebuffer_blit(uint32_t* source, uint32_t* destination, uint32_t width, uint32_t height) {
    size_t total_bytes = (size_t)width * height * sizeof(uint32_t);
    memcpy(destination, source, total_bytes);
}