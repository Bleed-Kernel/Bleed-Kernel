#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline void framebuffer_blit(uint32_t* source, uint32_t* destination,
                                    uint32_t width, uint32_t height, uint32_t pitch) {
    if (!source || !destination)
        return;
    for (uint32_t y = 0; y < height; y++) {
        memcpy(destination + ((size_t)y * pitch),
               source + ((size_t)y * pitch),
               (size_t)width * sizeof(uint32_t));
    }
}