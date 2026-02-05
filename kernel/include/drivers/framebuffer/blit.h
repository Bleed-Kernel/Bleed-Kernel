#pragma once

#include <stdint.h>
#include <stddef.h>

void framebuffer_blit(uint32_t* source, uint32_t* destination, uint32_t width, uint32_t height);