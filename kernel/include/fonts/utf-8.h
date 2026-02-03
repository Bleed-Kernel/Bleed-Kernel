#pragma once
#include <stddef.h>
#include <stdint.h>

size_t utf8_decode(const uint8_t *s, uint32_t *out);