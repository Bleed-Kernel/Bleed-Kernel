#pragma once

#include <fs/vfs.h>

int pipe_create_file_pair(file_t **out_read, file_t **out_write);
void pipe_file_release(file_t *f);
