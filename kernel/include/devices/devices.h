#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct INode INode_t;

#define MAX_DEVICES 64  // change this up later for now im avoiding flexable array members

long device_register(INode_t *device, char *name);
INode_t *device_get_by_name(const char *name);