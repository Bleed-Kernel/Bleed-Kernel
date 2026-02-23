#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

/// @brief initialse and test the serial port so its ready for writing
/// @return success
int serial_init();

/// @brief write a string to serial
/// @param str const string
void serial_write(const char* str);

/// @brief write a fixed-size buffer to serial
/// @param buf bytes to write
/// @param len number of bytes
void serial_write_n(const char *buf, size_t len);

/// @brief write a hex value to the screen from a uint
/// @param value uint value
void serial_write_hex(uint64_t value);

/// @brief Write a formatted string to COM1
/// @param fmt formatted string
/// @param  VARDIC
void serial_printf(const char* fmt, ...);

#endif
