#ifndef STDIO_H
#define STDIO_H 1

#include <string.h>
#include <stdint.h>

int snprintf(char *buf, size_t size, const char *fmt, ...);

/// @brief kernel tty print formatted string
/// @param fmt string
/// @param vardic argument list
void kprintf(const char *fmt, ...)__attribute__((format(printf,1,2)));

void kprintf_pos(uint64_t x, uint64_t y, const char *fmt, ...);

#endif