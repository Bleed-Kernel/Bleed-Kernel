#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct user_file{
    int permissions;
    size_t filesize;
    char fname[1024];
} user_file_t;