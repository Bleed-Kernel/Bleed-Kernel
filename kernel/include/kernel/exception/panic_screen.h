#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <kernel/exception/panic.h>

// read the logo and fonts in while its still safe to, true if the drawn panic is ready
bool panic_screen_load(void);

// loaded and the framebuffer is one it can draw on
bool panic_screen_ready(void);

// draw the panic over the whole screen with report as a qr code, sf is NULL when the kernel raised it itself
void panic_screen_draw(struct isr_stackframe *sf, const char *pstring, const char *report);
