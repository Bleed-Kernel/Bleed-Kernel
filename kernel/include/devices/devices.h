#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct INode INode_t;

#define DEVICES_GROW 64  // the list starts with room for this many and doubles when its full

long device_register(INode_t *device, char *name);
INode_t *device_get_by_name(const char *name);
const char *device_get_name(INode_t *inode);