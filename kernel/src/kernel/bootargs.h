#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char* key;
    char* value;
} bootarg_t;

void        bootargs_init(const char* cmdline);
const char* bootargs_get(const char* key);
bool        bootargs_is(const char* key, const char* expected_value);
bool        bootargs_has(const char* key);