#pragma once
#include <stdint.h>
#include <fs/vfs.h>

#define SERIAL_TTY_COLS     80
#define SERIAL_TTY_ROWS     24
#define SERIAL_TTY_EVENTS   256

struct task;

// /dev/ttyS0, a terminal on COM1. -ENODEV if theres no port
int serial_tty_init(void);

// start elf with ttyS0 as its stdin, stdout and stderr
struct task *serial_tty_spawn(INode_t *elf);
