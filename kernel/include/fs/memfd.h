#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <fs/vfs.h>
#include <mm/paging.h>

#define MFD_CLOEXEC         0x1
#define MFD_ALLOW_SEALING   0x2

#define F_SEAL_SEAL         0x1
#define F_SEAL_SHRINK       0x2
#define F_SEAL_GROW         0x4
#define F_SEAL_WRITE        0x8
#define F_SEAL_FUTURE_WRITE 0x10

// a new memfd in the lowest free fd, or a negative errno
int memfd_create(const char *name, unsigned int flags);

bool memfd_is(INode_t *inode);

// the frame behind page index of the memfd, made when create is set. 0 past the end
paddr_t memfd_page(INode_t *inode, uint64_t index, bool create);

int memfd_add_seals(INode_t *inode, uint32_t seals);
int memfd_get_seals(INode_t *inode);

// a writable shared mapping came or went, F_SEAL_WRITE cant go on while there are any
int memfd_track_writer(INode_t *inode, int delta);
