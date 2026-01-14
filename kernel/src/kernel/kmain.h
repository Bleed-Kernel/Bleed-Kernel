#pragma once
#include <devices/type/tty_device.h>

extern tty_t tty0;

void kernel_self_test();
tty_t kernel_console_init();

/*
    THIS IS A PROTOTYPE AND SHOULD NOT BE IN KERNEL SPACE
    WILL BE MOVED TO SHELL SOON!

    REMEMBER REMOVE PRINTF LINE 111
*/

void prototype_clock();