#pragma once

#include <stdint.h>
#include <stddef.h>

void framebuffer_blit(uint32_t* source, uint32_t* destination, uint32_t width, uint32_t height) {
    size_t size = width * height * sizeof(uint32_t);
    for (size_t i = 0; i < size / 8; i++) {
        ((uint64_t*)destination)[i] = ((uint64_t*)source)[i];
    }

    size_t rem = size % 8;
    uint8_t* s = (uint8_t*)source + (size - rem);
    uint8_t* d = (uint8_t*)destination + (size - rem);
    for (size_t i = 0; i < rem; i++)
        d[i] = s[i];
}