#pragma once

#include <devices/devices.h>

int tty_ioctl(INode_t *dev, unsigned long req, void *arg);
int tty_write(INode_t *dev, const void *buf, size_t len);
long tty_read(INode_t *dev, void *buf, size_t len, size_t offset);
