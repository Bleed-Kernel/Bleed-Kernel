#pragma once

#include <fs/vfs.h>

int pipe_create_file_pair(file_t **out_read, file_t **out_write);
void pipe_file_release(file_t *f);

// whichever end f is stops blocking
void pipe_set_nonblock(file_t *f);
